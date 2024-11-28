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

#include "packet.h"
#include "common.h"

// Constants
#define STDIN_FD 0          	// Standard input file descriptor
#define RETRY 120           	// Timeout duration in milliseconds
#define WINDOW_SIZE 10      	// Size of sliding window

/*
 * Structure to track each packet in the sending window
 * Contains:
 * - Pointer to the packet data
 * - Flags for tracking packet state
 * - Timestamp of when packet was sent
 */
typedef struct
{
	tcp_packet *packet;   		// Pointer to actual packet data
	bool is_sent;         		// Flag indicating if packet has been sent
	bool is_acked;        		// Flag indicating if packet has been acknowledged
	struct timeval sent_time; 	// Time when packet was last sent
} window_slot;


/*
 * Structure to track duplicate acknowledgments
 * Used for implementing fast retransmit
 */
struct
{
	int ack_number; // ACK number being tracked
	int count;  	// Number of times this ACK has been received
} dup_ack_tracker;

// Function declarations
void init_sender_window();
void start_timer();
void stop_timer();
void reset_timer();
void send_packet(int slot);
void process_ack(tcp_packet *ack_pkt);
void resend_packets(int sig);
void init_timer(int delay, void (*sig_handler)(int));


// Global variables
window_slot sender_window[WINDOW_SIZE]; // Array of window slots
int send_base = 0;                  	// First unacked packet sequence number
int next_seqno = 0;                 	// Next sequence number to use
int sockfd;                         	// Socket file descriptor
socklen_t serverlen;                	// Length of server address
struct sockaddr_in serveraddr;      	// Server address structure
struct itimerval timer;             	// Timer for packet retransmission
sigset_t sigmask;                   	// Signal mask for timer management
bool timer_running = false;         	// Flag to track timer state

/*
 * Initialize the sender's window buffer
 * Sets all slots to empty state
 */
void init_sender_window()
{
	for (int i = 0; i < WINDOW_SIZE; i++)
	{
    	sender_window[i].packet = NULL;
    	sender_window[i].is_sent = false;
    	sender_window[i].is_acked = false;
	}
}

/*
 * Calculate buffer slot for a sequence number
 * Implements circular buffer using modulo operation
 */
int get_window_slot(int seqno)
{
	return (seqno/DATA_SIZE) % WINDOW_SIZE;
}

/*
 * Check if sending window is full
 * Window is full if distance between next_seqno and send_base equals WINDOW_SIZE
 */
bool window_is_full()
{
	if (next_seqno >= send_base + (WINDOW_SIZE * DATA_SIZE)) {
    	return true;
	}
	return false;
}

bool is_valid_window_state() {
	if (next_seqno < send_base) return false;
	if (next_seqno - send_base > WINDOW_SIZE * DATA_SIZE) return false;
	return true;
}

/*
 * Timer management functions
 */
// Start timer if not already running
void start_timer()
{
	if (!timer_running)
	{
    	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    	setitimer(ITIMER_REAL, &timer, NULL);
    	timer_running = true;
	}
}

// Stop timer if running
void stop_timer()
{
	if (timer_running)
	{
    	sigprocmask(SIG_BLOCK, &sigmask, NULL);
    	timer_running = false;
	}
}

// Reset timer by stopping and starting it
void reset_timer()
{
	stop_timer();
	start_timer();
}

/*
 * Send a packet and update related state
 * Handles:
 * - Packet transmission
 * - Timer management
 * - State updates
 */
void send_packet(int slot)
{
	if (sendto(sockfd, sender_window[slot].packet,
			   TCP_HDR_SIZE + get_data_size(sender_window[slot].packet), 0,
			   (const struct sockaddr *)&serveraddr, serverlen) < 0)
	{
		error("sendto failed");
	}

	// Update packet state
	sender_window[slot].is_sent = true;
	gettimeofday(&sender_window[slot].sent_time, NULL);

	// Start timer for first unacked packet
	if (next_seqno == send_base)
	{
		start_timer();
	}

	VLOG(DEBUG, "Sent packet %d to %s", sender_window[slot].packet->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
}

/*
 * Process received acknowledgment
 * Handles:
 * - Duplicate ACK detection
 * - Fast retransmit after 3 duplicate ACKs
 * - Window advancement
 * - Buffer management
 */
void process_ack(tcp_packet *ack_pkt)
{
	int ack_no = ack_pkt->hdr.ackno; // Extract acknowledgment number
	VLOG(DEBUG, "Recieved ACK %d", ack_no);

	// Prevent some sort of race condition
	stop_timer();

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
				// Fast retransmit for the packet causing duplicate ACKs
				int slot = get_window_slot(ack_no);
				if (sender_window[slot].packet != NULL && !sender_window[slot].is_acked)
				{
					send_packet(slot);
					VLOG(DEBUG, "Fast retransmitted packet %d", ack_no);
				}
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
			sender_window[slot].is_acked = true;
		}
	}

	// Get the size of the last acknowledged packet
	int data_len = get_data_size(sender_window[get_window_slot(ack_no)].packet);

	// Advance send_base (sliding window)
	send_base = ack_no + data_len;
	VLOG(DEBUG, "Advanced send_base to %d", send_base);

	// This is when all packets have been acknowledged (base has caught up to next_seqno)
	if (send_base == next_seqno)
	{
		stop_timer(); 
	}

	// Restart timer for remaining unacknowledged packets
	else
	{
		reset_timer(); 
	}
}

/*
 * Handle timeout events
 * Retransmits the all packets sent starting from oldest unacked packet till the latest sent packet
 */
void resend_packets(int sig)
{
	if (sig == SIGALRM)
	{
    	VLOG(INFO, "Timeout occurred for packet: %d. Retransmitting...", send_base);

    	for (int i = 0; i < WINDOW_SIZE; i++)
    	{
        	int seqno = send_base + i * DATA_SIZE; // Calculate the sequence number for this slot
        	if (seqno >= next_seqno)           	// Stop if we have iterated beyond the last sent packet
            	break;

        	int slot = get_window_slot(seqno);
        	if (sender_window[slot].packet != NULL && !sender_window[slot].is_acked)
        	{
            	// Retransmit the unacknowledged packet
            	if (sendto(sockfd, sender_window[slot].packet,
                       	TCP_HDR_SIZE + get_data_size(sender_window[slot].packet), 0,
                       	(const struct sockaddr *)&serveraddr, serverlen) < 0)
            	{
                	error("sendto failed during retransmission");
            	}

            	gettimeofday(&sender_window[slot].sent_time, NULL);
            	VLOG(DEBUG, "Retransmitted packet %d", seqno);
        	}
    	}

    	// Reset the timer after retransmitting all unacknowledged packets
    	reset_timer();
	}
}

/*
 * Initialize timer for packet timeout detection
 * Parameters:
 * delay: Timeout duration in milliseconds
 * sig_handler: Function to call on timeout
 */
void init_timer(int delay, void (*sig_handler)(int))
{
	signal(SIGALRM, sig_handler);

	// Set up timer intervals
	timer.it_interval.tv_sec = delay / 1000;
	timer.it_interval.tv_usec = (delay % 1000) * 1000;
	timer.it_value.tv_sec = delay / 1000;
	timer.it_value.tv_usec = (delay % 1000) * 1000;

	// Initialize signal mask
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGALRM);
}

int main(int argc, char **argv)
{
	int portno;
	char *hostname;
	char buffer[DATA_SIZE];
	FILE *fp;
	int len;

	/* check command line arguments */
	if (argc != 4) {
    	fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
    	exit(1);
	}
    
	hostname = argv[1];
	portno = atoi(argv[2]);
	fp = fopen(argv[3], "r");
	if (fp == NULL) {
    	error(argv[3]);
	}

	/* socket: create the socket */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0)
    	error("ERROR opening socket");

	/* initialize server server details */
	bzero((char *)&serveraddr, sizeof(serveraddr));
	serverlen = sizeof(serveraddr);

	/* covert host into network byte order */
	if (inet_aton(hostname, &serveraddr.sin_addr) == 0)
	{
    	fprintf(stderr, "ERROR, invalid host %s\n", hostname);
    	exit(0);
	}

	/* build the server's Internet address */
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(portno);

	/*
	An assertion is made to check that the MSS (Maximum Segment Size) minus the
	TCP header size is greater than zero. This ensures that there is enough room
	in the data packet for TCP header information.
	*/
	assert(MSS_SIZE - TCP_HDR_SIZE > 0);

	init_sender_window();

	// Stop and wait protocol ------------------------------------------------
	// Initialize the timer with the specified retry duration and the resend_packets function as the signal handler.
	init_timer(RETRY, resend_packets);

	// infinite loop continues till end of file is reached
	while (1)
	{
		printf("\n");
		
		// Clean up and exit if invalid window state detected
		if (!is_valid_window_state()) {
        	VLOG(INFO, "Invalid window state detected: send_base = %d next_seqno = %d", send_base, next_seqno);
        	fclose(fp);
        	return -1;  // Return with error
    	}

    	fd_set readfds;
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 50000; 
		int no_of_slots_left;

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

		// if(activity == 0) {
		// 	printf("Select Call timed out...\n");
		// }

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
			no_of_slots_left = WINDOW_SIZE - ((next_seqno - send_base) / DATA_SIZE);

			// Log new window state
			VLOG(DEBUG, "Window Status: Oldest unACKed seqno = %d | Next seqno = %d | Datasize = %d | Number of slots left: %d",
				 send_base, next_seqno, len, no_of_slots_left);
		}

		// If the window is not full, send more packets
    	if (!window_is_full())
    	{
        	// Read up to DATA_SIZE bytes from the file pointed to by fp into the buffer.
        	len = fread(buffer, 1, DATA_SIZE, fp);

			// If it len <= 0 then it means either it reached (EOF) or encounters an error.
        	if (len <= 0) {
				// If  EOF (completed sending all the data successfully:
				if (feof(fp)) {
					VLOG(INFO, "End Of File has been reached");

					// Wait for all packets to be acknowledged
                	while (send_base < next_seqno - DATA_SIZE) {
                    	fd_set readfds;
						FD_ZERO(&readfds);
                    	FD_SET(sockfd, &readfds);

						// Wait for any ACKs
						if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
							(struct sockaddr *)&serveraddr, &serverlen) < 0) {
							error("recvfrom failed");
						}
						tcp_packet *ack_pkt = (tcp_packet *)buffer;
						if (ack_pkt->hdr.ackno > send_base) {
							process_ack(ack_pkt);
						}
                	}

					// Send EOF packet only after all data is acknowledged
					VLOG(INFO, "All packets acknowledged, starting EOF handshake");

					tcp_packet *eof_pkt = make_packet(0); // Create EOF packet
					eof_pkt->hdr.seqno = next_seqno;	  // Assign sequence number
					eof_pkt->hdr.ctr_flags = 0x02;		  // Set FIN flag

					while (1)
					{
						// Send EOF packet
						sendto(sockfd, eof_pkt, TCP_HDR_SIZE, 0,
							   (const struct sockaddr *)&serveraddr, serverlen);

						VLOG(DEBUG, "EOF packet %d sent, waiting for acknowledgment", eof_pkt->hdr.seqno);

						// Wait for ACK with timeout
						fd_set readfds;
						struct timeval timeout;
						timeout.tv_sec = 0; 
						timeout.tv_usec = 50000;
						FD_ZERO(&readfds);
						FD_SET(sockfd, &readfds);

						int rv = select(sockfd + 1, &readfds, NULL, NULL, &timeout);

						if (rv > 0)
						{
							// Wait to receive final ACK
							char buffer[MSS_SIZE];
							recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, &serverlen);
							tcp_packet *ack_pkt = (tcp_packet *)buffer;

							if (ack_pkt->hdr.ctr_flags == ACK && ack_pkt->hdr.ackno == next_seqno)
							{
								VLOG(INFO, "EOF acknowledgment received, terminating sender");
								break;
							}
						}
						else
						{
							VLOG(INFO, "EOF acknowledgment not received, retransmitting EOF");
						}
					}

					free(eof_pkt); // Clean up EOF packet
					fclose(fp);	   // Close file
					return 0;
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
				no_of_slots_left = WINDOW_SIZE - ((next_seqno - send_base) / DATA_SIZE);

				VLOG(DEBUG, "Window Status: Oldest unACKAed seqno = %d | Next seqno = %d | Datasize = %d | Number of slots left: %d", send_base, next_seqno, len, no_of_slots_left);
        	}
    	}

    	else {
			VLOG(DEBUG, "Window Status: FULL");
    	}
	}

	// Clean up any remaining packets in the window
	for (int i = 0; i < WINDOW_SIZE; i++)
	{
    	if (sender_window[i].packet != NULL)
    	{
        	free(sender_window[i].packet);
        	sender_window[i].packet = NULL;
    	}
	}

	fclose(fp);
	return 0;
}