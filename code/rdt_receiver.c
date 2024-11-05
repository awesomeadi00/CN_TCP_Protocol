/*
rdt_receiver.c : implements a receiver program that listens for incoming 
UDP datagrams (packets) from the sender. It reads the data from the received packets,
writes it to a file, and sends acknowledgments back to the sender. 
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"


/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */

//2 pointers to tcp_packet structures to be used to handle received and sent packets.
tcp_packet *recvpkt;
tcp_packet *sndpkt;

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    //open file in writing mode to write the received data
    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     main loop: wait for a datagram, then echo it
     Receiver waits for incoming packets from the sender
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    
    //loop till end of file is reached
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        //VLOG(DEBUG, "waiting from server \n");
        
        //receive data stored into the buffer from the sender
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }

        //received data cast to tcp_packet
        recvpkt = (tcp_packet *) buffer;

        //check if received data size is within expected limits
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        //check if data_size field in the received packet's header is 0
        //if it is 0, then means end of file reached, so terminate loop
        if ( recvpkt->hdr.data_size == 0) {
            //VLOG(INFO, "End Of File has been reached");
            //close file pointer
            fclose(fp);
            break;
        }
        /* 
        sendto: ACK back to the client
        After receiving a data packet, the receiver sends ACK back to the sender
         */

        //get the current time, log it for debugging purposes
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        //received data is written to the file fp at the specified position
        fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
        fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
        
        //An ACK packet is created and prepared to be sent back to the sender
        sndpkt = make_packet(0);

        //set ackno field in the ACK packet to the sequence number of received data + data size
        sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
        
        //set the ctlr flags to ACK to indicate it's an acknowledgment packet
        sndpkt->hdr.ctr_flags = ACK;

        //send the ACK packet back to the sender using sendto
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
    }
    //When loop ends after end of file reached, file is closed, receiver program terminates
    

    return 0;
}

//check some online animation tools to visualize a similar process:
//https://www2.tkn.tu-berlin.de/teaching/rn/animations/gbn_sr/

