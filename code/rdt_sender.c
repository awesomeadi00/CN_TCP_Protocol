/*
rdt_sender.c simulates the behavior of a sender in a reliable data transfer protocol,
ensuring that data is transmitted, acknowledged, and retransmitted if necessary
while handling errors and timeouts.
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include"packet.h"
#include"common.h"

//define file descriptor for standard input which is 0
#define STDIN_FD    0

//deine the timeout duration in milliseconds for retransmitting packets
#define RETRY  120 //millisecond

//keeps track of the sequence number for the next packet to be sent
int next_seqno=0;

//represents the sequence number of the base packet in the sender's window.
int send_base=0;

//size of the sender's window (initially set to 1)
int window_size = 1;

//socekt file descriptor , and length of server's address structure
int sockfd, serverlen;

//server's address struct
struct sockaddr_in serveraddr;

//stores information about the timer used for retransmissions.
struct itimerval timer; 

//pointer to reference TCP packet structure for sending data
tcp_packet *sndpkt;

//pointer to reference TCP packet structure for receiving data
tcp_packet *recvpkt;

//signal mask used for signal handling
sigset_t sigmask;       


/*
function resend_packets is a signal handler for SIGALRM, which is triggered when
the timer expires.
It checks if the signal is indeed SIGALRM (indicating a timeout).
If it is a timeout, it logs a message, indicating that a timeout has occurred,
and resends the packet sndpkt to the server using the sendto function.
*/
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timout happend");
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
    }
}


/*
The start_timer function is used to start the timer for packet timeouts.
It unblocks the SIGALRM signal using sigprocmask.
It sets the timer to the values stored in the timer struct using setitimer.
*/
void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

//The stop_timer function is used to stop the timer by blocking the SIGALRM signal
//using sigprocmask.
void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/* The init_timer function is used to initialize the timer for packet timeouts.
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    //set up the signal handler for SIGALRM using signal.
    signal(SIGALRM, sig_handler);
    
    //configure the timer values based on the specified delay in milliseconds,
    //setting both the initial and interval values. This sets the timer to trigger
    //the SIGALRM signal after the specified delay and then repeatedly at the
    //specified interval.
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /*
    check 3 command-line arguments: 
    the hostname of the server, the portno to connect to, and the name
    of a file to be read as the data source for transmission.
     */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
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
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
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

    
    //Stop and wait protocol


    //initialize the timer with the specified retry duration and
    //the resend_packets function as the signal handler.
    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    
    //infinite loop continues till end of file is reached
    while (1)
    {
        //data is read from the file fp into the buffer with a maximum length of DATA_SIZE
        len = fread(buffer, 1, DATA_SIZE, fp);
        
        //if no data read, would indicate end of file
        if ( len <= 0)
        {
            //print a log message
            VLOG(INFO, "End Of File has been reached");
            
            //a special packet with data size 0 is created & sent to server to signal
            //the end of the transmission
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            break;
        }


        //MAIN DATA TRANSMISSION SECTION OF LOOP

        //send_base and next_seqno keep track of the sender's window,
        //and window_size is initially set to 1.

        //send_base is set to the current next_seqno
        send_base = next_seqno;

        //next_seqno is updated by adding the length of the data just read.
        next_seqno = send_base + len;

        //A packet is created using make_packet with the length of data read.
        sndpkt = make_packet(len);

        //The data is copied from the buffer into the packet
        memcpy(sndpkt->data, buffer, len);

        //The sequence number is set
        sndpkt->hdr.seqno = send_base;

        
        /*
        This section of code is responsible for reliably sending data packets and
        waiting for corresponding ACKs from the server. It uses a timeout mechanism
        to ensure reliable data transmission. If the expected ACK is not received
        within the specified timeout (RETRY), the sender will resend the packet
        and continue waiting for the ACK until it is received.
        This ensures the reliability of the Stop-and-Wait protocol.
        */
        do {

            VLOG(DEBUG, "Sending packet %d to %s", 
                    send_base, inet_ntoa(serveraddr.sin_addr));
            
            /* packet is sent to server using sendto()
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            //timer is started to monitor whether an ACK is received within a 
            //specific time frame (the retry duration)
            start_timer();
           
            //enter a loop to wait for an ACK
            do
            {
                //data was sent using sendto earlier. 
                //now waits for a response using recevfrom.
                //this is a blocking call that waits for data to arrive on the socket
                //the received data is stored in buffer 
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                {
                    error("recvfrom");
                }

                //the received data is cast to a tcp_packet struct
                recvpkt = (tcp_packet *)buffer;
                printf("%d \n", get_data_size(recvpkt));
                
                //check if the size of the received data is within expected limits
                assert(get_data_size(recvpkt) <= DATA_SIZE);
            }while(recvpkt->hdr.ackno < next_seqno);    //ignore duplicate ACKs
            /*
            The loop continues to execute until the received ACK's acknowledgment
            number (recvpkt->hdr.ackno) is greater than or equal to the expected
            acknowledgment number (next_seqno). This is important because the
            sender may receive duplicate ACKs, and we want to ignore those duplicates.
            */

            //When the expected ACK is received or the loop exits, the sender stops
            //the timer using stop_timer().
            stop_timer();
            
            /*resend pack if don't recv ACK */
        } while(recvpkt->hdr.ackno != next_seqno);      
        /*
        The outer loop continues to resend the packet and wait for an acknowledgment
        as long as the received ACK number (recvpkt->hdr.ackno) is not equal to
        the expected ACK number (next_seqno). This ensures that the sender retransmits
        the packet in case an ACK is not received or if there is a timeout.
        */

        /*
        deallocate memory that was previously allocated for the sndpkt structure
        It's a good practice to free dynamically allocated memory when it's no longer
        needed to avoid memory leaks and conserve system resources.
        */
        free(sndpkt);
    }

    return 0;

}





