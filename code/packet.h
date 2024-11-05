/*
enum is used to define enumeration type called "packet_type" here. 
Enumerations are a way to define a set of named integer constants. 
Here, DATA and ACK are the two enumerator constants defined. 

DATA: This constant represents a packet type that is used for transmitting data. 
When a packet has the DATA type, it typically contains actual data payload that 
needs to be transmitted from the sender to the receiver.

ACK: This constant represents a packet type used for acknowledgment. 
An acknowledgment packet (ACK) is typically sent by the receiver to acknowledge
the successful reception of a data packet. It confirms that a specific data packet
has been received and processed successfully.

Enumerations are often used to make code more readable and maintainable by giving
meaningful names to integer constants. In this case, DATA and ACK provide clear 
and self-explanatory names for the two types of packets used in the protocol. 
It's a way to make the code more human-friendly and less error-prone compared to using
raw integer constants.
*/

enum packet_type {
    DATA,
    ACK,
};


/*
seqno: This field represents the sequence number. In the context of a TCP packet, 
the sequence number is used to identify the position of the data within a stream of bytes.
It helps in reordering and reassembling packets at the receiver's end.

ackno: This field represents the acknowledgment number. It is used in TCP to
acknowledge the receipt of data. The acknowledgment number indicates the
sequence number of the next expected byte from the sender.

ctr_flags: This field represents control flags. It is used to convey control
information or flags associated with the packet. Common TCP flags include 
SYN (synchronize), ACK (acknowledgment)...

data_size: This field represents the size of the data payload carried by the TCP packet.
It indicates the length of the data in bytes.
*/
typedef struct {
    int seqno;
    int ackno;
    int ctr_flags;
    int data_size;
}tcp_header;


/*MSS refers to the Maximum Segment Size, which is the maximum amount of data that can
/be encapsulated in a single TCP segment (packet) without fragmentation. The value 1500
indicates the typical Maximum Transmission Unit (MTU) for Ethernet networks, which is
the largest size a packet can be without being fragmented.

UDP_HDR_SIZE: This constant defines the size of the UDP (User Datagram Protocol) header
in bytes. UDP is a transport layer protocol, and its header size is typically 8 bytes.

IP_HDR_SIZE: This constant defines the size of the IP (Internet Protocol) header in bytes.
The IP header is used for routing and addressing information. The size is typically
20 bytes for IPv4.

TCP_HDR_SIZE: This constant defines the size of the TCP (Transmission Control Protocol)
header in bytes. It is calculated using the sizeof(tcp_header) expression,
which evaluates to the size of the tcp_header struct defined. 

DATA_SIZE: This constant calculates the maximum data payload size that can be included
in a TCP packet while adhering to the specified MSS. It subtracts the sizes of 
the TCP header, UDP header, and IP header from the MSS size to determine the available
space for data payload. 
*/
#define MSS_SIZE    1500
#define UDP_HDR_SIZE    8
#define IP_HDR_SIZE    20
#define TCP_HDR_SIZE    sizeof(tcp_header)
#define DATA_SIZE   (MSS_SIZE - TCP_HDR_SIZE - UDP_HDR_SIZE - IP_HDR_SIZE)


/*
hdr (tcp_header): This field is an instance of the tcp_header struct, which represents
the header of the TCP packet. It includes fields like seqno, ackno, ctr_flags,
and data_size to store control and sequence information.

data: This field is a flexible array member. It allows the tcp_packet struct to carry
a variable-length data payload. The size of the data payload is not fixed in the
struct definition and can vary based on the actual data being transmitted.
*/
typedef struct {
    tcp_header  hdr;
    char    data[0];
}tcp_packet;

//function to make packet
tcp_packet* make_packet(int seq);

//function to get data size of the packet
int get_data_size(tcp_packet *pkt);
