#include <stdlib.h>
#include"packet.h"

/*
This line declares a static variable zero_packet of type tcp_packet. It is initialized
with a structure initializer that sets all its fields, including the hdr field, to zero.
This is a common practice to initialize a "zeroed" packet that can be used as a template
for creating new packets.
*/
static tcp_packet zero_packet = {.hdr={0}};


/*
Function to create a TCP packet with a specified data length.
It takes len as an argument, representing the length of the data payload to be
included in the packet.
*/
tcp_packet* make_packet(int len)
{
    tcp_packet *pkt; //A pointer pkt of type tcp_packet
    
    /*Memory allocated using malloc to create a packet with a total size
    of TCP_HDR_SIZE + len. This ensures enough space for the TCP header and data payload.
    */
    pkt = malloc(TCP_HDR_SIZE + len); 

    //packet pointed to by pkt is initialized with the zero_packet template, 
    //ensuring that all fields are zeroed.
    *pkt = zero_packet;

    //The hdr.data_size field of the packet is set to the specified len to indicate
    //the size of the data payload.
    pkt->hdr.data_size = len;

    //A pointer to the created packet is returned.
    return pkt;
}

/*
function that takes a pointer to a TCP packet as an argument and returns the size 
of the data payload stored in the packet's header.
*/
int get_data_size(tcp_packet *pkt)
{
    return pkt->hdr.data_size;
}

