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
	tcp_packet *packet;   	// Pointer to actual packet data
	bool is_sent;         	// Flag indicating if packet has been sent
	bool is_acked;        	// Flag indicating if packet has been acknowledged
	struct timeval sent_time; // Time when packet was last sent
} window_slot;

/*
 * Structure to maintain window state information
 * Tracks:
 * - Various sequence numbers for window management
 * - Duplicate ACK handling
 */
struct
{
	int LastPacketAcked; 	// Sequence number of last acknowledged packet
	int LastPacketSent;  	// Sequence number of last sent packet
	int LastPacketAvailable; // Highest sequence number available to send
	int duplicate_ack_count; // Count of duplicate ACKs received
	int last_ack_received;   // Most recent ACK number received
} window_state;

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
void init_window_state();
void init_sender_window();
void start_timer();
void stop_timer();
void reset_timer();
void send_packet(int slot);
void process_ack(tcp_packet *ack_pkt);
void resend_packets(int sig);
void init_timer(int delay, void (*sig_handler)(int));

/*
 * Initialize the window state variables
 * Sets all counters and sequence numbers to initial values
 */
void init_window_state()
{
	window_state.LastPacketAcked = 0;
	window_state.LastPacketSent = 0;
	window_state.LastPacketAvailable = 0;
	window_state.duplicate_ack_count = 0;
	window_state.last_ack_received = -1;
	dup_ack_tracker.ack_number = -1;
	dup_ack_tracker.count = 0;
}

/*
 * Check if current window state meets all constraints:
 * 1. LastPacketAcked <= LastPacketSent
 * 2. LastPacketSent <= LastPacketAvailable
 * 3. Window size limit not exceeded
 */
bool window_constraints_valid()
{
	return (window_state.LastPacketAcked <= window_state.LastPacketSent &&
        	window_state.LastPacketSent <= window_state.LastPacketAvailable &&
        	window_state.LastPacketSent - window_state.LastPacketAcked <= WINDOW_SIZE);
}

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
    	VLOG(DEBUG, "Timer started for packet %d", send_base);
	}
}

// Stop timer if running
void stop_timer()
{
	if (timer_running)
	{
    	sigprocmask(SIG_BLOCK, &sigmask, NULL);
    	timer_running = false;
    	VLOG(DEBUG, "Timer stopped");
	}
}

// Reset timer by stopping and starting it
void reset_timer()
{
	stop_timer();
	start_timer();
	VLOG(DEBUG, "Timer reset for packet %d", send_base);
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
	int ack_no = ack_pkt->hdr.ackno;

	// Ignore ACKs for packets already acknowledged
	if (ack_no <= send_base) {
    	VLOG(DEBUG, "Received duplicate or outdated ACK %d", ack_no);
    	return;
	}

	if (ack_no > next_seqno) {
    	VLOG(DEBUG, "Received invalid ACK %d beyond next_seqno %d", ack_no, next_seqno);
    	return;
	}

	// Update window state
	window_state.LastPacketAcked = ack_no;
	window_state.last_ack_received = ack_no;
	window_state.duplicate_ack_count = 0;

	// Free acknowledged packets
	while (send_base < ack_no) {
    	int slot = get_window_slot(send_base);
    	if (sender_window[slot].packet != NULL) {
        	free(sender_window[slot].packet);
        	sender_window[slot].packet = NULL;
        	sender_window[slot].is_acked = true;
        	sender_window[slot].is_sent = false;
    	}
    	send_base += DATA_SIZE;
	}

	// Reset or stop timer
	if (send_base < next_seqno) {
    	reset_timer();
	} else {
    	stop_timer();
	}
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
 * Handle timeout events
 * Retransmits the all packets sent starting from oldest unacked packet till the latest sent packet
 */
void resend_packets(int sig)
{
	if (sig == SIGALRM)
	{
    	VLOG(INFO, "Timeout occurred. Retransmitting all unacknowledged packets");

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
	init_window_state();   

	// Stop and wait protocol

	// initialize the timer with the specified retry duration and
	// the resend_packets function as the signal handler.
	init_timer(RETRY, resend_packets);
	next_seqno = 0;

	// infinite loop continues till end of file is reached
	while (1)
	{
    	if (!is_valid_window_state()) {
        	VLOG(INFO, "Invalid window state detected: send_base=%d next_seqno=%d",
             	send_base, next_seqno);
        	// Clean up and exit
        	fclose(fp);
        	return -1;  // Return with error
    	}

    	fd_set readfds;
    	struct timeval timeout;
    	timeout.tv_sec = 0;
    	timeout.tv_usec = 1000; // 1ms timeout
    	int no_of_slots_left = WINDOW_SIZE - ((next_seqno - send_base) / DATA_SIZE);
    	bool full_alert = false;

    	FD_ZERO(&readfds);
    	FD_SET(sockfd, &readfds); // Monitor socket for incoming ACKs

    	// Check for incoming ACKs
    	int activity;
    	do
    	{
        	activity = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
    	} while (activity < 0 && errno == EINTR);

    	if (activity < 0)
    	{
        	error("select failed");
    	}

    	// If the window is not full, send more packets
    	if (!window_is_full())
    	{
        	full_alert = false;
        	// Read up to DATA_SIZE bytes from the file pointed to by fp into the buffer.
        	// if it lem <= 0 then it means either it reached (EOF) or encounters an error.
        	len = fread(buffer, 1, DATA_SIZE, fp);

        	// Check for EOF (completed sending all the data successfully) or read errors
        	// If data transferred successfully then exit
        	if (len <= 0) {
            	if (feof(fp)) {
                	VLOG(INFO, "Reached end of file");
               	 
                	// Wait for all packets to be acknowledged
                	while (send_base < next_seqno) {
                    	// Check for ACKs
                    	fd_set readfds;
                    	struct timeval timeout;
                    	timeout.tv_sec = 0;
                    	timeout.tv_usec = 1000;
                   	 
                    	FD_ZERO(&readfds);
                    	FD_SET(sockfd, &readfds);
                   	 
                    	if (select(sockfd + 1, &readfds, NULL, NULL, &timeout) > 0) {
                        	if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                            	(struct sockaddr *)&serveraddr, &serverlen) < 0) {
                            	error("recvfrom failed");
                        	}
                        	tcp_packet *ack_pkt = (tcp_packet *)buffer;
                        	if (ack_pkt->hdr.ackno > send_base) {
                            	process_ack(ack_pkt);
                        	}
                    	}
                	}
               	 
                	// Send EOF packet only after all data is acknowledged
                	VLOG(INFO, "All packets acknowledged, sending EOF");
                	tcp_packet *eof_pkt = make_packet(0);
                	for (int i = 0; i < 3; i++) {  // Send multiple times for reliability
                    	sendto(sockfd, eof_pkt, TCP_HDR_SIZE, 0,
                           	(const struct sockaddr *)&serveraddr, serverlen);
                	}
                	free(eof_pkt);
                	fclose(fp);
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

            	// Copy data and set header fields (set seqno)
            	memcpy(sender_window[slot].packet->data, buffer, len);
            	sender_window[slot].packet->hdr.seqno = next_seqno;

            	// Send packet
            	send_packet(slot);

            	// Update sequence number by len as packets are varied in size hence this will account for packet size transfer
            	next_seqno += len;

            	VLOG(DEBUG, "Window Status After Packet-Send: Oldest unACKAed seqno = %d | Next seqno = %d | Datasize = %d | Number of slots left: %d", send_base, next_seqno, next_seqno - send_base, no_of_slots_left);
        	}
    	}

    	else {
        	if (!full_alert)
        	{
            	VLOG(DEBUG, "Window Status: FULL");
            	full_alert = true;
        	}
    	}

    	// Process ACKs if data is available on the socket
    	if (FD_ISSET(sockfd, &readfds))
    	{
        	// ACK received
        	if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, &serverlen) < 0)
        	{
            	error("recvfrom failed");
        	}

        	tcp_packet *ack_pkt = (tcp_packet *)buffer;

        	// Process ack if the ack number is above the oldest seqno packet (ignores packets which have been acked)
        	if (ack_pkt->hdr.ackno > send_base)
        	{
            	process_ack(ack_pkt);

            	// Log new window state
            	VLOG(DEBUG, "Window Status After ACK: Oldest unACKed seqno = %d | Next seqno = %d | Datasize = %d | Number of slots left: %d",
                 	send_base, next_seqno, next_seqno - send_base, no_of_slots_left);
        	}
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