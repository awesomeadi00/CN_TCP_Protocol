/*
rdt_sender.c: Implementation of a reliable data transfer sender using sliding window protocol.
Key features:
- Implements sliding window with size of 10 packets
- Handles packet retransmission on timeout
- Processes cumulative acknowledgments
- Implements fast retransmit after 3 duplicate ACKs
- Manages timer for unacknowledged packets
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <math.h>

#include "packet.h"
#include "common.h"

// Constants
#define STDIN_FD 0		   // Standard input file descriptor
#define MAX_WINDOW_SIZE 64 // Maximum size of sliding window (64)

// Structures: --------------------------------------------------------------------------------------------------------------------------------------------------------------
// The sender window and its features
typedef struct
{
	tcp_packet *packet;		  // Pointer to actual packet data
	bool is_sent;			  // Flag indicating if packet has been sent
	bool is_acked;			  // Flag indicating if packet has been acknowledged
	bool is_retransmitted;	  // Flag indicating if the packet has been retransmitted
	struct timeval sent_time; // Time when packet was last sent
} window_slot;

// Structure to track duplicate acknowledgments (implemented for fast-tracking)
struct
{
	int ack_number; // ACK number being tracked
	int count;		// Number of times this ACK has been received
} dup_ack_tracker;

// Function declarations ----------------------------------------------------------------------------------------------------------------------------------------------------
void init_sender_window();
int get_window_slot(int seqno);
bool window_is_full();
bool is_valid_window_state();

void log_cwnd(double time);
void init_cwnd_log();
void close_cwnd_log();
void update_cwnd(struct timeval current_time);
void reset_congestion_control();
void handle_fast_retransmit(int ack_no);

void init_timer(float delay, void (*sig_handler)(int));
void start_timer(bool restartTimer);
void stop_timer(bool restartTimer);
void reset_timer();

void update_rtt_and_rto(struct timeval sent_time, bool is_retransmitted);
void send_packet(int slot);
void process_ack(tcp_packet *ack_pkt);
void resend_packets(int sig);
void initiate_eof_handshake(FILE *fp, char *buffer);

// Global variables -----------------------------------------------------------------------------------------------------------------------------------------------------------
window_slot sender_window[MAX_WINDOW_SIZE]; // Array of window slots
int send_base = 0;							// First unacked packet sequence number
int next_seqno = 0;							// Next sequence number to use
int sockfd;									// Socket file descriptor
socklen_t serverlen;						// Length of server address
struct sockaddr_in serveraddr;				// Server address structure
struct itimerval timer;						// Timer for packet retransmission
sigset_t sigmask;							// Signal mask for timer management
bool timer_running = false;					// Flag to track timer state
bool window_full_gate = false;				// Gate to prevent FULL status from printing

// Global variables for RTO and RTT calculations:
float wrtt = 0.0;		  // Weighted average of RTT values (wrtt = (1-alpha) * wrtt + (alpha * sample_rtt))
float devrtt = 0.0;			  // Deviation in RTT (devrtt = (1-beta) * devrtt + beta * |wrtt - sample_rtt|)
float alpha = 0.125;		  // Coefficient for weighted average RTT
float beta = 0.25;			  // Coefficient for deviation in RTT
float rto = 3.0;			  // Retransmission Timeout (RTO), initialized to 3 seconds
int consecutive_timeouts = 0; // Counter for exponential backoff

// Global variables for Congestion Control:
// Congestion Control State to determine whether we are in Slow start or Congestion Avoidance
typedef enum
{
	SLOW_START,
	CONGESTION_AVOIDANCE
} CongestionControlState;
CongestionControlState congestionState = SLOW_START;

float cwnd = 1.0;	   // Congestion window (set to 1 packet initially)
float ssthresh = 64.0; // Slow-start threshold (set to 64 initially)
FILE *cwnd_log;		   // CSV File for logging CWND

// Window Related Helper Functions ------------------------------------------------------------------------------------------------------------------------------------------------
// Initialize the sender's window buffer and sets all slots to empty state
void init_sender_window()
{
	for (int i = 0; i < MAX_WINDOW_SIZE; i++)
	{
		sender_window[i].packet = NULL;
		sender_window[i].is_sent = false;
		sender_window[i].is_acked = false;
		sender_window[i].is_retransmitted = false;
	}
}

// Calculate buffer slot for a sequence number and implements circular buffer using modulo operation
int get_window_slot(int seqno)
{
	return (seqno / DATA_SIZE) % MAX_WINDOW_SIZE;
}

// Check if sending window is full. Window is full if distance between next_seqno and send_base equals floor of the CWND
bool window_is_full()
{
	if (next_seqno >= send_base + ((int)floor(cwnd) * DATA_SIZE))
	{
		return true;
	}
	return false;
}

// Function to check if the window is in a valid state. It is not if the seqno is < send_base
bool is_valid_window_state()
{
	// next_seqno cannot be < send_base
	if (next_seqno < send_base)
		return false;

	// The packet cannot exceed the entire window range
	if (next_seqno - send_base > MAX_WINDOW_SIZE * DATA_SIZE)
		return false;

	return true;
}

// Congestion Control and CWND Logging Helper Functions: ---------------------------------------------------------------------------------------------------------------------------------------
// Log CWND to CSV file
void log_cwnd(double time)
{
	// Logging it as TIME, CWND, SSTRESH on the CSV file
	if (cwnd_log != NULL)
	{
		fprintf(cwnd_log, "%.6f,%.6f,%d\n", time, cwnd, (int)ssthresh);
		fflush(cwnd_log);
	}
}

// Initialize CWND logging
void init_cwnd_log()
{
	cwnd_log = fopen("../CWND.csv", "w");
	if (cwnd_log == NULL)
	{
		error("Failed to create CWND.csv");
	}
}

// Close CWND logging
void close_cwnd_log()
{
	if (cwnd_log != NULL)
	{
		fclose(cwnd_log);
	}
}

// Updates CWND based on ACK and depending on the congestion control state
void update_cwnd(struct timeval current_time)
{
	double timestamp = current_time.tv_sec + current_time.tv_usec / 1e6;

	// If we are in slow start:
	if (congestionState == SLOW_START)
	{
		cwnd += 1.0; // Increment CWND linearly

		// If the window is more than the ssthresh, then we can switch to congestion avoidance.
		if (cwnd >= ssthresh)
		{
			congestionState = CONGESTION_AVOIDANCE;
			VLOG(INFO, "- Congestion Update: Switching to Congestion Avoidance");
		}
	}

	// If we are already in congestion avoidance
	else if (congestionState == CONGESTION_AVOIDANCE)
	{
		cwnd += 1.0 / cwnd; // Increment CWND incrementally
	}

	VLOG(INFO, "- Congestion Update: CWND: %.3f, SSTRESH: %.3f", cwnd, ssthresh);
	log_cwnd(timestamp); // Log CWND
}

// Handles congestion control reset under a timeout: Updates ssthresh value and CWND is reset
void reset_congestion_control()
{
	ssthresh = fmax(cwnd / 2, 2.0); // Adjust ssthresh
	cwnd = 1.0;						// Reset CWND to 1
	congestionState = SLOW_START;	// Transition to Slow Start
	VLOG(INFO, "- Congestion Update: Packet loss! CWND reset to %.3f, SSTHRESH set to %.3f", cwnd, ssthresh);
	VLOG(INFO, "- Congestion Update: Switching to Slow Start");
}

// Handles fast retransmit. Sends the packet once more, readjusting the ssthresh and CWND and switching back to SLOW START
void handle_fast_retransmit(int ack_no)
{
	int slot = get_window_slot(ack_no);
	if (sender_window[slot].packet != NULL)
	{
		send_packet(slot);				// Retransmit
		VLOG(INFO, "- Fast retransmit packet %d", ack_no);
	}
}

// Timer Related Helper Functions: ------------------------------------------------------------------------------------------------------------------------------------------
// Initializes the timer for packet timeout detection Includes the RTO and the resend_function handler
void init_timer(float delay, void (*sig_handler)(int))
{
	signal(SIGALRM, sig_handler);

	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 0;
	timer.it_value.tv_sec = (int)delay;
	timer.it_value.tv_usec = (delay - (int)delay) * 1e6;

	// Initialize signal mask
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGALRM);
}

// Start timer if not already running
void start_timer(bool restartTimer)
{
	if (!timer_running)
	{
		sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
		setitimer(ITIMER_REAL, &timer, NULL);
		timer_running = true;
	}

	if(!restartTimer)
		VLOG(DEBUG, "Timer Started!");
}

// Stop timer if running
void stop_timer(bool restartTimer)
{
	if (timer_running)
	{
		sigprocmask(SIG_BLOCK, &sigmask, NULL);
		timer_running = false;
	}

	if(!restartTimer)
		VLOG(DEBUG, "Timer Stopped!");
}

// Reset timer by stopping and starting it
void reset_timer()
{
	stop_timer(true);
	start_timer(true);
	VLOG(DEBUG, "Timer Restarted - new send_base allocated.");
}

// RTT and RTO Calculation Functions: -------------------------------------------------------------------------------------------------------------------------------------------
// Updates the RTT and RTO values dynamically
void update_rtt_and_rto(struct timeval sent_time, bool is_retransmitted)
{
	// Apply Karn's Algorithm: ignore RTT for retransmitted packets
	if (is_retransmitted)
	{
		VLOG(DEBUG, "- RTT/RTO Update: Skipping RTT update for retransmitted packet.");
		return;
	}

	// Get the ACK time
	struct timeval ack_time;
	gettimeofday(&ack_time, NULL);

	// Calculate RTT in seconds (Time the ACKed was received - Time the packet was sent)
	float sample_rtt = (ack_time.tv_sec - sent_time.tv_sec) +
					   (ack_time.tv_usec - sent_time.tv_usec) / 1e6;

	if (sample_rtt > 0)
	{
		// If this is the first RTT measurement
		if (wrtt == 0)
		{
			wrtt = sample_rtt;
			devrtt = sample_rtt / 2;
		}

		//  We calculate the deviation of RTT and weighted average of RTT based on the formula
		else
		{
			devrtt = (1 - beta) * devrtt + beta * fabs(wrtt - sample_rtt);
			wrtt = (1 - alpha) * wrtt + alpha * sample_rtt;
		}

		// Calculate RTO
		rto = wrtt + 4 * devrtt;

		// Clamp RTO to allowable range (1 second to 240 seconds)
		if (rto < 1.0)
			rto = 1.0;
		if (rto > 240.0)
			rto = 240.0;

		VLOG(DEBUG, "- RTT/RTO Update: sample_rtt = %.3f w_rtt = %.3f dev_rtt = %.3f rto = %.3f",
			 sample_rtt, wrtt, devrtt, rto);
	}
}

// Packet Transmission and Processing Functions: -----------------------------------------------------------------------------------------------------------------------------
// Function to send a packet. Includes packet transmission, timer management and state updates
void send_packet(int slot)
{
	printf("\n");

	if (sendto(sockfd, sender_window[slot].packet,
			   TCP_HDR_SIZE + get_data_size(sender_window[slot].packet), 0,
			   (const struct sockaddr *)&serveraddr, serverlen) < 0)
	{
		error("sendto failed");
	}

	VLOG(DEBUG, "Sent packet %d to %s", sender_window[slot].packet->hdr.seqno, inet_ntoa(serveraddr.sin_addr));

	// Update packet state
	sender_window[slot].is_sent = true;
	sender_window[slot].is_acked = false;
	sender_window[slot].is_retransmitted = false;

	gettimeofday(&sender_window[slot].sent_time, NULL);

	// Start timer for first unacked packet
	if (next_seqno == 0 && send_base == 0)
	{
		start_timer(false);
	}
}

// Function to process received ACKS
/*
 * Handles:
 * - Updates RTT and RTO
 * - Duplicate ACK detection
 * - Fast retransmit after 3 duplicate ACKs
 * - Window advancement
 * - Cumulative ACK receive
 * - Resets timer
 */
void process_ack(tcp_packet *ack_pkt)
{
	printf("\n");

	int ack_no = ack_pkt->hdr.ackno; // Extract acknowledgment number

	// If we receive an ack which is below the base (Duplicate ACK)
	if (ack_no < send_base)
	{
		VLOG(DEBUG, "Received duplicate ACK for %d", ack_no);

		// Handle duplicate ACKs for fast retransmit only if 3 duplicates are detected
		if (dup_ack_tracker.ack_number == ack_no)
		{
			dup_ack_tracker.count++;
			if (dup_ack_tracker.count == 3)
			{
				VLOG(DEBUG, "- Triple duplicate ACK Detected for %d", ack_no);

				// Reset CWND and SSTRESH due to triple duplicate ACK
				reset_congestion_control();
				
				// Fast retransmit the lost packet
				handle_fast_retransmit(send_base);
				dup_ack_tracker.count = 0; // Reset duplicate ACK counter
			}
		}
		else
		{
			dup_ack_tracker.ack_number = ack_no;
			dup_ack_tracker.count = 1;
		}
		return; // No further processing needed for duplicate ACK
	}

	// Otherwise this is a regular ontime ACK hence we will:
	// - Reset consecutive_timeouts to zero and update the timer
	// - Update RTT and RTO Values
	// - Gradually update the CWND value
	// - Clear window slot buffers
	// - Advance sliding window
	VLOG(DEBUG, "Recieved ACK %d", ack_no);

	int slot = get_window_slot(ack_no);
	consecutive_timeouts = 0;
	update_rtt_and_rto(sender_window[slot].sent_time, sender_window[slot].is_retransmitted);

	// Get the current time for logging the updated CWND value
	struct timeval current_time;
	gettimeofday(&current_time, NULL);
	update_cwnd(current_time);

	// Determine the size of the last acknowledged packet before freeing it
	int data_len = 0;
	if (sender_window[slot].packet != NULL)
	{
		data_len = get_data_size(sender_window[slot].packet);
	}

	// Handle cumulative ACK
	int num_acked = (ack_no - send_base) / DATA_SIZE;
	for (int i = 0; i <= num_acked; i++)
	{
		int slot = get_window_slot(send_base + i * DATA_SIZE);

		// Free memory for acknowledged packets
		if (sender_window[slot].packet != NULL)
		{
			free(sender_window[slot].packet);
			sender_window[slot].packet = NULL;
			sender_window[slot].is_sent = false;
			sender_window[slot].is_acked = true;
			sender_window[slot].is_retransmitted = false;
		}
	}

	// Advance send_base (sliding window)
	send_base = ack_no + data_len;
	VLOG(DEBUG, "Advanced send_base to %d", send_base);

	// Restart timer for remaining unacknowledged packets
	reset_timer();
}

// Resend packets: occurs when timeout occurs based on RTO
// Retransmits the all packets sent starting from oldest unacked packet till the latest sent packet
void resend_packets(int sig)
{
	if (sig == SIGALRM)
	{
		printf("\n");

		// Exponential backoff: Exponential double of RTO when timouts occur (with a ceiling of 240)
		consecutive_timeouts++;
		rto = fmin(rto * pow(2, consecutive_timeouts), 240.0);

		VLOG(INFO, "Timeout occurred for packet: %d. New RTO: %.3f", send_base, rto);

		// Only retransmit the packet that was timed out (not the whole window - according to project description)
		int slot = get_window_slot(send_base);
		if (sender_window[slot].packet != NULL && !sender_window[slot].is_acked)
		{
			// Retransmit the unacknowledged packet
			if (sendto(sockfd, sender_window[slot].packet,
						TCP_HDR_SIZE + get_data_size(sender_window[slot].packet), 0,
						(const struct sockaddr *)&serveraddr, serverlen) < 0)
			{
				error("sendto failed during retransmission");
			}
			sender_window[slot].is_retransmitted = true;

			gettimeofday(&sender_window[slot].sent_time, NULL);
			VLOG(DEBUG, "Retransmitted packet %d", send_base);
		}

		// Reset congestion values (CWND and SSTRESH)
		// reset_congestion_control();

		// Reset the timer after retransmitting all unacknowledged packets
		reset_timer();
	}
}

// Final function to indicate the end of file has been reached and we will initialize the EOF handshake
void initiate_eof_handshake(FILE *fp, char *buffer)
{
	printf("\n");
	VLOG(INFO, "End Of File has been reached");

	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(sockfd, &readfds);

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 50000;

	// Wait for all packets to be acknowledged
	while (send_base < next_seqno)
	{
		FD_ZERO(&readfds);
		FD_SET(sockfd, &readfds);
		
		int activity = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
		if (activity < 0)
		{
			if (errno == EINTR)
			{
				// Interrupted by a signal such as the retransmission (e.g., SIGALRM), retry select
				continue;
			}
			else
			{
				perror("select failed");
				exit(EXIT_FAILURE);
			}
		}

		if (activity > 0 && FD_ISSET(sockfd, &readfds))
		{
			// Wait for any ACKs
			if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
						 (struct sockaddr *)&serveraddr, &serverlen) < 0)
			{
				error("recvfrom failed");
			}

			tcp_packet *ack_pkt = (tcp_packet *)buffer;
			process_ack(ack_pkt);
		}
	}

	// Stop timer as all packets have been acknowledged
	stop_timer(false);

	// Send EOF packet only after all data is acknowledged
	VLOG(INFO, "All packets acknowledged, starting EOF handshake");

	tcp_packet *eof_pkt = make_packet(0); // Create EOF packet
	eof_pkt->hdr.seqno = next_seqno;	  // Assign sequence number
	eof_pkt->hdr.ctr_flags = 0x02;		  // Set FIN flag

	// Loop max_attempts to send the EOF packet if not confirmed
	int max_attempts = 3; 
	int attempts = 0;
	while (attempts < max_attempts)
	{
		// Send EOF packet
		sendto(sockfd, eof_pkt, TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen);

		VLOG(DEBUG, "EOF packet %d sent, waiting for acknowledgment", eof_pkt->hdr.seqno);

		// Wait for ACK with timeout
		int activity = select(sockfd + 1, &readfds, NULL, NULL, &timeout);

		if (activity > 0)
		{
			// Process final ACK
			if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, &serverlen) > 0)
			{
				tcp_packet *ack_pkt = (tcp_packet *)buffer;

				if (ack_pkt->hdr.ctr_flags == ACK && ack_pkt->hdr.ackno == next_seqno)
				{
					VLOG(INFO, "EOF acknowledgment received, sender terminating.");
					break;
				}
			}
		}
		else
		{
			VLOG(INFO, "EOF acknowledgment not received, retransmitting EOF");
		}

		attempts++;
	}

	if (attempts == max_attempts)
	{
		VLOG(DEBUG, "Failed to receive EOF acknowledgment after multiple attempts, sender terminating.");
	}

	free(eof_pkt); 	// Clean up EOF packet
}

// MAIN FUNCTION -----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–-----–---
int main(int argc, char **argv)
{
	int portno;
	char *hostname;
	char buffer[DATA_SIZE];
	FILE *fp;
	int len;

	// Check command line arguments
	if (argc != 4)
	{
		fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
		exit(1);
	}

	hostname = argv[1];
	portno = atoi(argv[2]);
	fp = fopen(argv[3], "r");
	if (fp == NULL)
	{
		error(argv[3]);
	}

	// Socket: create the socket
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0)
		error("ERROR opening socket");

	// Iitialize server server details
	bzero((char *)&serveraddr, sizeof(serveraddr));
	serverlen = sizeof(serveraddr);

	// Covert host into network byte order
	if (inet_aton(hostname, &serveraddr.sin_addr) == 0)
	{
		fprintf(stderr, "ERROR, invalid host %s\n", hostname);
		exit(0);
	}

	// Build the server's internet address
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(portno);

	/*
	An assertion is made to check that the MSS (Maximum Segment Size) minus the
	TCP header size is greater than zero. This ensures that there is enough room
	in the data packet for TCP header information.
	*/
	assert(MSS_SIZE - TCP_HDR_SIZE > 0);

	init_sender_window();
	init_cwnd_log();

	// Stop and wait protocol ------------------------------------------------
	// Initialize the timer with the specified retry duration and the resend_packets function as the signal handler.
	init_timer(rto, resend_packets);

	// Infinite loop continues till end of file is reached
	while (1)
	{
		// Clean up and exit if invalid window state detected
		if (!is_valid_window_state())
		{
			VLOG(INFO, "Invalid window state detected: send_base = %d next_seqno = %d", send_base, next_seqno);
			fclose(fp);
			return -1; // Return with error
		}

		fd_set readfds;
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 50000;

		// Monitor socket for incoming ACKs
		FD_ZERO(&readfds);
		FD_SET(sockfd, &readfds);

		int activity = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
		if (activity < 0)
		{
			if (errno == EINTR)
			{
				// Interrupted by a signal such as the retransmission (e.g., SIGALRM), retry select
				continue;
			}
			else
			{
				perror("select failed");
				exit(EXIT_FAILURE);
			}
		}

		// Process ACKs if data is available on the socket
		if (activity > 0 && FD_ISSET(sockfd, &readfds))
		{
			// Receive an ACK from the reciever
			if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, &serverlen) < 0)
			{
				error("recvfrom failed");
			}

			tcp_packet *ack_pkt = (tcp_packet *)buffer;
			process_ack(ack_pkt);

			// Log new window state
			VLOG(DEBUG, "Window Status: send_base = %d | next_seqno = %d | cwnd_slots: %d",
				 send_base, next_seqno, (int)floor(cwnd));
		}

		// If the window is not full, send more packets
		if (!window_is_full())
		{
			window_full_gate = false;

			// Read up to DATA_SIZE bytes from the file pointed to by fp into the buffer.
			len = fread(buffer, 1, DATA_SIZE, fp);

			// If it len <= 0 then it means either it reached (EOF) or encounters an error.
			if (len <= 0)
			{
				// If  EOF (completed sending all the data successfully:
				if (feof(fp))
				{
					initiate_eof_handshake(fp, buffer);
					break;
				}
				error("Error reading from file");
			}

			// Otherwise, we will continue to prepare and send more packets within the window
			else
			{
				// Get the slot number within the window for the packet
				int slot = get_window_slot(next_seqno);

				// Clean up old packet if it exists
				if (sender_window[slot].packet != NULL)
				{
					free(sender_window[slot].packet);
				}

				// Create new packet based on the size of the data payload (len)
				sender_window[slot].packet = make_packet(len);
				if (sender_window[slot].packet == NULL)
				{
					error("Failed to create packet");
				}

				// Assign sequence number to the packet
				memcpy(sender_window[slot].packet->data, buffer, len);
				sender_window[slot].packet->hdr.seqno = next_seqno;

				// Send packet
				send_packet(slot);

				// Update sequence number by len as packets are varied in size hence this will account for packet size transfer
				next_seqno += len;

				VLOG(DEBUG, "Window Status: send_base = %d | next_seqno = %d | datasize = %d | cwnd_slots: %d",
					 send_base, next_seqno, len, (int)floor(cwnd));
			}
		}

		else
		{
			// Just to avoid it printing over and over again.
			if(!window_full_gate) {
				VLOG(DEBUG, "Window Status: FULL - Cannot send more packets");
				window_full_gate = true;
			}
			
		}
	}

	// Clean up any remaining packets in the window
	for (int i = 0; i < MAX_WINDOW_SIZE; i++)
	{
		if (sender_window[i].packet != NULL)
		{
			free(sender_window[i].packet);
			sender_window[i].packet = NULL;
		}
	}

	// Close CWND logging file and cleanup
	close_cwnd_log();
	fclose(fp);
	return 0;
}