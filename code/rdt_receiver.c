/*
rdt_receiver.c : Implementation of a reliable data transfer receiver using sliding window protocol.
Key features:
- Supports window size of 10 packets
- Handles out-of-order packet buffering
- Sends cumulative acknowledgments
- Maintains packet ordering using sequence numbers
- Writes received data to file in correct order
*/

// Online animation for visualizing the entire process
// https://www2.tkn.tu-berlin.de/teaching/rn/animations/gbn_sr/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"

#define WINDOW_SIZE 4          	// Keep the window size <= max seq. no / 2

/*
 * Structure to represent a slot in the receiver's buffer
 * Each slot can hold:
 * - A pointer to a TCP packet (NULL if empty)
 * - A flag indicating if the slot contains valid data
 */
typedef struct {
	tcp_packet *packet;  	// Pointer to store the actual packet
	bool is_buffered;   		// Flag to track if slot contains valid data
} receiver_buffer_slot;


// Global variables for receiver state management
receiver_buffer_slot receiver_buffer[WINDOW_SIZE];  // Circular buffer for out-of-order packets
int rcv_base = 0;  	// Base sequence number - next expected in-order packet
int highest_seqno = 0; // Highest sequence number seen so far

/*
 * Initialize the receiver's circular buffer
 * Sets all slots to empty (NULL packet pointer and invalid flag)
 */
void init_receiver_buffer() {
	for(int i = 0; i < WINDOW_SIZE; i++) {
    	receiver_buffer[i].packet = NULL;	// No packet
		receiver_buffer[i].is_buffered = false; // Slot is empty
	}
}

/*
 * Calculate the buffer slot for a given sequence number
 * Uses modulo operation to implement circular buffer behavior
 * Returns: slot index in the buffer array
 */
int get_buffer_slot(int seqno) {
	return (seqno/DATA_SIZE) % WINDOW_SIZE;
}


/*
 * Send an acknowledgment packet back to the sender
 *
 * Parameters:
 * sockfd: Socket descriptor for sending
 * ackno: Acknowledgment number (next expected sequence number)
 * addr: Sender's address structure
 * addr_len: Length of sender's address structure
 */
void send_ack(int sockfd, int ackno, struct sockaddr_in *addr, socklen_t addr_len) {
	// Create an empty ACK packet
	tcp_packet *ack_pkt = make_packet(0);
	ack_pkt->hdr.ackno = ackno;     	// Set ACK number
	ack_pkt->hdr.ctr_flags = ACK;   	// Set ACK flag
    
	// Log ACK details for debugging
	VLOG(DEBUG, "Sending ACK %d to client %s:%d",
     	ackno,
     	inet_ntoa(addr->sin_addr),
     	ntohs(addr->sin_port));

	// Send the ACK packet
	if(sendto(sockfd, ack_pkt, TCP_HDR_SIZE, 0,
          	(struct sockaddr *)addr, addr_len) < 0) {
    	error("ERROR in sendto");
	}
    
	// Clean up
	free(ack_pkt);
}

/*
 * Process packets that are ready to be delivered to the application
 * Writes consecutive packets to file starting from rcv_base
 * Stops when it encounters a gap in sequence numbers
 */
void process_buffered_packets(FILE *fp)
{
	while (1)
	{
		int slot = get_buffer_slot(rcv_base);

		// Stop if we find a packet that is not buffered
		if (!receiver_buffer[slot].is_buffered)
		{
			break;
		}

		VLOG(DEBUG, "Processing buffered packet: %d", receiver_buffer[slot].packet->hdr.seqno);

		// Write this packet's data to file at the correct position
		fseek(fp, rcv_base, SEEK_SET);
		fwrite(receiver_buffer[slot].packet->data,
			   1, receiver_buffer[slot].packet->hdr.data_size, fp);

		// Update rcv_base to next expected sequence number
		rcv_base += receiver_buffer[slot].packet->hdr.data_size;

		// Clean up the buffer slot
		free(receiver_buffer[slot].packet);
		receiver_buffer[slot].packet = NULL;
		receiver_buffer[slot].is_buffered = false;
	}
}

/*
 * Main function implementing the receiver logic
 * Handles:
 * - Socket setup
 * - Packet reception
 * - Buffer management
 * - File writing
 * - ACK sending
 */
int main(int argc, char **argv) {
	int sockfd;                			// Socket	
	int portno;							// Port to listen on
	socklen_t clientlen;             	// Size of client's address
	struct sockaddr_in serveraddr; 		// Server's address structure
	struct sockaddr_in clientaddr; 		// Client's address structure
	int optval;                			// Socket option value
	FILE *fp;                  			// Output file pointer
	char buffer[MSS_SIZE];     			// Buffer for receiving packets
	struct timeval tp;         			// Timestamp structure

	/* check command line arguments */
	if (argc != 3) {
    	fprintf(stderr, "usage: %s <port> <FILE_RECVD>\n", argv[0]);
    	exit(1);
	}
    
	portno = atoi(argv[1]);
	fp = fopen(argv[2], "w");
	if (fp == NULL) {
    	error(argv[2]);
	}

	// Create UDP socket
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0)
    	error("ERROR opening socket");

	// Set socket option to reuse address
	// This allows quick restart of the server
	optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
        	(const void *)&optval , sizeof(int));

	// Initialize server address structure
	bzero((char *) &serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;       	// Internet address family
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); // Accept on any interface
	serveraddr.sin_port = htons((unsigned short)portno); // Set port number

	// Bind socket to server address
	if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
    	error("ERROR on binding");

	// Initialize receiver state
	init_receiver_buffer();
	clientlen = sizeof(clientaddr);

	// Start logging
	VLOG(DEBUG, "Waiting for Packets...");

	// Main packet processing loop
	while (1) {
		printf("\n");
		
    	// Receive a packet
    	if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                	(struct sockaddr *)&clientaddr, &clientlen) < 0) {
        	error("ERROR in recvfrom");
    	}

    	// Cast received data to packet structure
    	tcp_packet *received_pkt = (tcp_packet *)buffer;

		// Log client information
		VLOG(DEBUG, "Received packet %d from client %s:%d",
			received_pkt->hdr.seqno,
			inet_ntoa(clientaddr.sin_addr),
			ntohs(clientaddr.sin_port));

		// Check for EOF packet (marked by zero data size)
    	if(received_pkt->hdr.data_size == 0) {
        	VLOG(INFO, "EOF Packet %d received. End of file has been reached", received_pkt->hdr.seqno);
       	 
        	// Process any remaining buffered packets
			process_buffered_packets(fp);

			send_ack(sockfd, received_pkt->hdr.seqno, &clientaddr, clientlen);

			VLOG(INFO, "Final ACK sent, receiver terminating.");
        	break;
    	}

    	// Verify packet size is within limits
    	assert(get_data_size(received_pkt) <= DATA_SIZE);

    	// Log packet details
    	gettimeofday(&tp, NULL);
    	VLOG(DEBUG, "Received at (epoch): %lu | Data size: %d | Seqno: %d", tp.tv_sec,
         	received_pkt->hdr.data_size, received_pkt->hdr.seqno);

    	// If a packet that arrived is before the receive base, it is a re-transmitted packet. 
		// Hence, we send the ACK once more to the sender. 
    	if (received_pkt->hdr.seqno < rcv_base)
    	{
			VLOG(DEBUG, "Retransmission received for seqno %d (already ACKed). Re-sending ACK.", received_pkt->hdr.seqno);

			// Re-send the acknowledgment for the corresponding packet
			send_ack(sockfd, received_pkt->hdr.seqno, &clientaddr, clientlen);

			continue; // Skip further processing for this packet
		}

    	// Process received packet based on its sequence number

		// In-order packet - write directly to file
		if(received_pkt->hdr.seqno == rcv_base) {
        	fseek(fp, rcv_base, SEEK_SET);
        	fwrite(received_pkt->data, 1, received_pkt->hdr.data_size, fp);

			// Send cumulative acknowledgment
			send_ack(sockfd, rcv_base, &clientaddr, clientlen);

			// Update rcv_base
			rcv_base += received_pkt->hdr.data_size;
       	 
        	// Check if we can now process any buffered packets
			process_buffered_packets(fp);
		}

		// Out-of-order packet - store in buffer
		else if(received_pkt->hdr.seqno > rcv_base) {
        	int slot = get_buffer_slot(received_pkt->hdr.seqno);

			// If the packet has not been buffered
			if (!receiver_buffer[slot].is_buffered)
			{
				// Allocate space and store packet
            	receiver_buffer[slot].packet = malloc(TCP_HDR_SIZE + received_pkt->hdr.data_size);
            	memcpy(receiver_buffer[slot].packet, received_pkt, TCP_HDR_SIZE + received_pkt->hdr.data_size);
				receiver_buffer[slot].is_buffered = true;

				// Update highest sequence number seen
            	if(received_pkt->hdr.seqno > highest_seqno) {
                	highest_seqno = received_pkt->hdr.seqno;
            	}
				VLOG(DEBUG, "Out of order packet received, buffered packet: %d", received_pkt->hdr.seqno);
			}

			// Otherwise it has been buffered, hence we will discard it. 
			else {
				VLOG(DEBUG, "Out-of-order packet %d already buffered - discarded", received_pkt->hdr.seqno);
			}
		}

		VLOG(DEBUG, "Updated receive_base to %d", rcv_base);
	}

	// Clean up and exit
	fclose(fp);
	return 0;
}