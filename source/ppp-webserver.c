// PPP-Blinky - "The Most Basic Internet Thing"

// Copyright 2016/2017 Nicolas Nackel aka Nixnax. Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// Notes and Instructions
// http://bit.ly/PPP-Blinky-Instructions
// http://bit.ly/win-rasdial-config

// Handy reading material
// https://technet.microsoft.com/en-us/library/cc957992.aspx
// https://en.wikibooks.org/wiki/Serial_Programming/IP_Over_Serial_Connections
// http://atari.kensclassics.org/wcomlog.htm

// Connecting PPP-Blinky to Linux
// PPP-Blinky can be made to talk to Linux - tested on Fedora - the following command, which uses pppd, works:
// pppd /dev/ttyACM0 115200 debug dump local passive noccp novj nodetach nocrtscts 172.10.10.1:172.10.10.2
// in the above command 172.10.10.1 is the adapter IP, and 172.10.10.2 is the IP of PPP-Blinky.
// See also https://en.wikipedia.org/wiki/Point-to-Point_Protocol_daemon

// Special pages when PPP-Blinky is running
// 172.10.10.2  root page
// 172.10.10.2/x  returns the number of ppp frames sent - this is handy for testing
// 172.10.10.2/xb  also returns number of ppp frames sent, but issues a fast refresh meta command. This allows you to use your browser to benchmark page load speed
// 172.10.10.2/ws  a simple WebSocket demo
// http://jsfiddle.net/d26cyuh2/  more complete WebSocket demo in JSFiddle, showing cross-domain access

#include "SerialManager.h"
#include <stdio.h>
#include <string.h>
#include "sha1.h"
#include "MKW41Z4.h"
#include "LED.h"
#include "genfsk.h"
#include "ppp-webserver.h"
#include "genfsk_defs.h"


const static char rootWebPage[] = "\
<!DOCTYPE html>\
<html>\
<head>\
<title>Blinky Over Radio</title>\
<body style=\"font-family: sans-serif; font-size:25px; color:#807070\">\
<h1>Blinky Over Radio</h1>\
<form>\
<input type=\"button\" value=\"Toggle LED1\" onclick=\"window.location.href= \'/a\'\"/>\
<input type=\"button\" value=\"Toggle LED2\" onclick=\"window.location.href= \'/b\'\"/>\
<input type=\"button\" value=\"Toggle LED3\" onclick=\"window.location.href= \'/c\'\"/>\
</form>\
</body>\
</html>";

// Serial connection
uint8_t pc;

// the standard hdlc frame start/end character. It's the tilde character "~"
#define FRAME_7E (0x7e)

pppType ppp; // our global - definitely not thread safe

/// Initialize the ppp structure and clear the receive buffer
void pppInitStruct()
{
    memset( ppp.rx.buf, 0, RXBUFLEN);
    ppp.online=0;
    ppp.rx.tail=0;
    ppp.rx.rtail=0;
    ppp.rx.head=0;
    ppp.rx.buflevel=0;
    ppp.pkt.len=0;
    ppp.ipData.ident=10000; // easy to recognize in ip packet dumps
    ppp.ledState=0;
    ppp.hdlc.frameStartIndex=0;
    ppp.responseCounter=0;
    ppp.firstFrame=1;
    ppp.ppp = (pppHeaderType *)ppp.pkt.buf; // pointer to ppp header
    ppp.ip = (ipHeaderType *)(ppp.pkt.buf+4); // pointer to IP header
}

/// Returns 1 after a connect message, 0 at startup or after a disconnect message
int connectedPpp()
{
    return ppp.online;
}

/// Initialize the PPP FCS (frame check sequence) total
void fcsReset()
{
    ppp.fcs=0xffff;   // crc restart
}

/// update the cumulative PPP FCS (frame check sequence)
void fcsDo(int x)
{
    for (int i=0; i<8; i++) {
        ppp.fcs=((ppp.fcs&1)^(x&1))?(ppp.fcs>>1)^0x8408:ppp.fcs>>1; // crc calculator
        x>>=1;
    }
}

/// calculate the PPP FCS (frame check sequence) on an entire block of memory
int fcsBuf(char * buf, int size) // crc on an entire block of memory
{
    fcsReset();
    for(int i=0; i<size; i++)fcsDo(*buf++);
    return ppp.fcs;
}

/// Get one character from our received PPP buffer
int pc_getBuf()
{
    int x = ppp.rx.buf[ ppp.rx.tail ];
    ppp.rx.tail=(ppp.rx.tail+1)&(RXBUFLEN-1);
    ppp.rx.buflevel--;
    return x;
}

/// Process a received PPP frame
void processPPPFrame(int start, int end)
{
   
    if(start==end) {
        return; // empty frame
    }

    fcsReset();
    char * dest = ppp.pkt.buf;
    ppp.pkt.len=0;
    int unstuff=0;
    int idx = start;
    while(1) {
        if (unstuff==0) {
            if (ppp.rx.buf[idx]==0x7d) unstuff=1;
            else {
                *dest = ppp.rx.buf[idx];
                ppp.pkt.len++;
                dest++;
                fcsDo(ppp.rx.buf[idx]);
            }
        } else { // unstuff characters prefixed with 0x7d
            *dest = ppp.rx.buf[idx]^0x20;
            ppp.pkt.len++;
            dest++;
            fcsDo(ppp.rx.buf[idx]^0x20);
            unstuff=0;
        }
        idx = (idx+1) & (RXBUFLEN-1);
        if (idx == end) break;
    }
    ppp.pkt.crc = ppp.fcs & 0xffff;
    if (ppp.pkt.crc == 0xf0b8) { // check for good CRC
        determinePacketType();
    }
}

/// output a character to the PPP port while checking for incoming characters
void pcPutcWhileCheckingInput(uint8_t ch)
{
    Serial_SyncWrite(pc, &ch, 1);
}

/// do PPP HDLC-like handling of special (flag) characters
void hdlcPut(int ch)
{
    if ( (ch<0x20) || (ch==0x7d) || (ch==0x7e) ) {
        pcPutcWhileCheckingInput(0x7d);
        pcPutcWhileCheckingInput(ch^0x20);  // these characters need special handling
    } else {
        pcPutcWhileCheckingInput(ch);
    }
}

/// send a PPP frame in HDLC format
void sendPppFrame()
{
    ppp.responseCounter++; // count the number of ppp frames we send
    int crc = fcsBuf(ppp.pkt.buf, ppp.pkt.len-2); // update crc
    ppp.pkt.buf[ ppp.pkt.len-2 ] = (~crc>>0); // fcs lo (crc)
    ppp.pkt.buf[ ppp.pkt.len-1 ] = (~crc>>8); // fcs hi (crc)
    pcPutcWhileCheckingInput(0x7e); // hdlc start-of-frame "flag"
    for(int i=0; i<ppp.pkt.len; i++) {
        hdlcPut( ppp.pkt.buf[i] ); // send a character
    }
    pcPutcWhileCheckingInput(0x7e); // hdlc end-of-frame "flag"
}

/// convert a network ip address in the buffer to an integer (IP adresses are big-endian, i.e most significant byte first)
int bufferToIP(char * buffer)
{
    int result=0;
    for(int i=0; i<4; i++) result = (result<<8)|(*buffer++ & 0xff);
    return result;
}

/// convert 4-byte ip address to 32-bit
unsigned int ip( int a, int b, int c, int d)
{
    return a<<24 | b<<16 | c<<8 | d;
}

/// handle IPCP configuration requests
void ipcpConfigRequestHandler()
{
    if(ppp.ipcp->request[0]==3) {
        ppp.hostIP = bufferToIP(ppp.pkt.buf+10);
    }

    ppp.ipcp->code=2; // change code to ack
    sendPppFrame(); // acknowledge everything they ask for - assume it's IP addresses

    ppp.ipcp->code=1; // change code to request
    ppp.ipcp->lengthR = __REV16( 4 ); // 4 is minimum length - no options in this request
    ppp.pkt.len=4+4+2; // no options in this request shortest ipcp packet possible (4 ppp + 4 ipcp + 2 crc)
    sendPppFrame(); // send our request
}

/// Handle IPCP NACK by sending our suggested IP address if there is an IP involved.
/// This is how Linux responds to an IPCP request with no options - Windows assumes any IP address on the submnet is OK.
void ipcpNackHandler()
{
    if (ppp.ipcp->request[0]==3) { // check if the NACK contains an IP address parameter
        ppp.ipcp->code=1; // assume the NACK contains our "suggested" IP address
        sendPppFrame(); // let's request this IP address as ours
    }
}

/// process an incoming IPCP packet
void IPCPframe()
{
    int action = ppp.ipcp->code; // packet type is here
    switch (action) {
        case 1:
            ipcpConfigRequestHandler();
            break;
        case 3:
            ipcpNackHandler();
            break;
        default:
            break;
    }
}

/// perform a 16-bit checksum. if the byte count is odd, stuff in an extra zero byte.
unsigned int dataCheckSum(char * ptr, int len, int restart)
{
    unsigned int i,hi,lo;
    unsigned char placeHolder;
    if (restart) ppp.sum=0;
    if (len&1) {
        placeHolder = ptr[len];
        ptr[len]=0;  // if the byte count is odd, insert one extra zero byte is after the last real byte because we sum byte PAIRS
    }
    i=0;
    while ( i<len ) {
        hi = ptr[i++];
        lo = ptr[i++];
        ppp.sum = ppp.sum + ((hi<<8)|lo);
    }
    if (len&1) {
        ptr[len] = placeHolder;    // restore the extra byte we made zero
    }
    ppp.sum = (ppp.sum & 0xffff) + (ppp.sum>>16);
    ppp.sum = (ppp.sum & 0xffff) + (ppp.sum>>16); // sum one more time to catch any carry from the carry
    return ~ppp.sum;
}

/// perform the checksum on an IP header
void IpHeaderCheckSum()
{
    ppp.ip->checksumR=0; // zero the checsum in the IP header
    int len = 4 * ppp.ip->headerLength; // length of IP header in bytes
    unsigned int sum = dataCheckSum(ppp.ipStart,len,1);
    ppp.ip->checksumR = __REV16( sum ); // insert fresh checksum
}

/// swap the IP source and destination addresses
void swapIpAddresses()
{
    unsigned int tempHold;
    tempHold = ppp.ip->srcAdrR; // tempHold <- source IP
    ppp.ip->srcAdrR = ppp.ip->dstAdrR; // source <- dest
    ppp.ip->dstAdrR = tempHold; // dest <- tempHold*/
}

/// swap the IP source and destination ports
void swapIpPorts()
{
    int headerSizeIP    = 4 * (ppp.ip->headerLength); // calculate size of IP header
    char * ipSrcPort = ppp.ipStart + headerSizeIP + 0; // ip source port location
    char * ipDstPort = ppp.ipStart + headerSizeIP + 2; // ip destin port location
    char tempHold[2];
    memcpy(tempHold, ipSrcPort,2); // tempHold <- source
    memcpy(ipSrcPort,ipDstPort,2); // source <- dest
    memcpy(ipDstPort,tempHold, 2); // dest <- tempHold
}

/// Build the "pseudo header" required for UDP and TCP, then calculate its checksum
void checkSumPseudoHeader( unsigned int packetLength )
{
    // this header  contains the most important parts of the IP header, i.e. source and destination address, protocol number and data length.
    pseudoIpHeaderType pseudoHeader; // create pseudo header
    pseudoHeader.srcAdrR = ppp.ip->srcAdrR; // copy in ip source address
    pseudoHeader.dstAdrR = ppp.ip->dstAdrR; // copy in ip dest address
    pseudoHeader.zero = 0; // zero byte
    pseudoHeader.protocol = ppp.ip->protocol; // protocol number (udp or tcp)
    pseudoHeader.lengthR = __REV16( packetLength ); // size of tcp or udp packet
    dataCheckSum(pseudoHeader.start, 12, 1); // calculate this header's checksum
}

/// initialize an IP packet to send
void initIP (unsigned int srcIp, unsigned int dstIp, unsigned int srcPort, unsigned int dstPort, unsigned int protocol)
{
    ppp.ppp->address = 0xff;
    ppp.ppp->control = 3;
    ppp.ppp->protocolR = __REV16( 0x0021 );
    ppp.ip->version = 4;
    ppp.ip->headerLength = 5; // 5 words = 20 bytes
    ppp.ip->identR = __REV16(ppp.ipData.ident++); // insert our ident
    ppp.ip->dontFragment=1;
    ppp.ip->ttl=128;
    ppp.ip->protocol = protocol; // udp
    ppp.ip->srcAdrR = __REV(srcIp);
    ppp.ip->dstAdrR = __REV(dstIp);
    ppp.udpStart = ppp.ipStart + 20; // calculate start of udp header
    ppp.udp->srcPortR = __REV16(srcPort); // source port
    ppp.udp->dstPortR = __REV16(dstPort); // dest port
}


/// Build a UDP packet from scratch
void sendUdp(unsigned int srcIp, unsigned int dstIp, unsigned int srcPort, unsigned int dstPort, char * message,int msgLen)
{
    struct {
        unsigned int ipAll; // length of entire ip packet
        unsigned int ipHeader; // length of ip header
        unsigned int udpAll; // length of entire udp packet
        unsigned int udpData; // length of udp data segment
    } len;
    len.ipHeader = 20; // ip header length
    len.udpData = msgLen; // udp data size
    len.udpAll = len.udpData+8; // update local udp packet length
    len.ipAll = len.ipHeader + len.udpAll; // update IP Length
    initIP(srcIp, dstIp, srcPort, dstPort, 17); // init a UDP packet
    ppp.ip->lengthR = __REV16(len.ipAll); // update IP length in buffer
    ppp.udpStart = ppp.ipStart + len.ipHeader; // calculate start of udp header
    memcpy( ppp.udp->data, message, len.udpData ); // copy the message to the buffer
    ppp.udp->lengthR = __REV16(len.udpAll); // update UDP length in buffer
    ppp.pkt.len = len.ipAll+2+4; // update ppp packet length
    IpHeaderCheckSum();  // refresh IP header checksum
    checkSumPseudoHeader( len.udpAll ); // get the UDP pseudo-header checksum
    ppp.udp->checksumR = 0; // before TCP checksum calculations the checksum bytes must be set cleared
    unsigned int pseudoHeaderSum=dataCheckSum(ppp.udpStart,len.udpAll, 0); // continue the TCP checksum on the whole TCP packet
    ppp.udp->checksumR = __REV16( pseudoHeaderSum); // tcp checksum done, store it in the TCP header
    sendPppFrame(); // send the UDP message back
}

/// Process an incoming UDP packet.
/// If the packet starts with the string "echo " or "test" we echo back a special packet
void UDPpacket()
{
    struct {
        unsigned int all; // length of entire ip packet
        unsigned int header; // length of ip header
    } ipLength;

    struct {
        unsigned int all; // length of entire udp packet
        unsigned int data; // length of udp data segment
    } udpLength;

    ipLength.header = 4 * ppp.ip->headerLength; // length of ip header
    ppp.udpStart = ppp.ipStart + ipLength.header; // calculate start of udp header
    udpLength.all = __REV16( ppp.udp->lengthR ); // size of udp packet
    udpLength.data = udpLength.all - 8; // size of udp data

    int echoFound = !strncmp(ppp.udp->data,"echo ",5); // true if UDP message starts with "echo "
    int testFound = !strncmp(ppp.udp->data,"test" ,4); // true if UDP message starts with "test"
    if ( (echoFound) || (testFound)) { // if the UDP message starts with "echo " or "test" we answer back
        if (echoFound) {
            swapIpAddresses(); // swap IP source and destination
            swapIpPorts(); // swap IP source and destination ports
            memcpy(ppp.udp->data,"Got{",4); // in the UDP data modify "echo" to "Got:"
            int n=0;
            n=n+sprintf(n+ppp.udp->data+udpLength.data, "} UDP Server: PPP-Blinky\n"); // an appendix
            udpLength.data = udpLength.data + n; // update udp data size with the size of the appendix
            // we may have changed data length, update all the lengths
            udpLength.all    = udpLength.data+8; // update local udp packet length
            ipLength.all     = ipLength.header + udpLength.all; // update IP Length
            ppp.ip->lengthR  = __REV16(ipLength.all); // update IP length in buffer
            ppp.udp->lengthR = __REV16(udpLength.all); // update UDP length in buffer
            ppp.pkt.len      = ipLength.all+2+4; // update ppp packet length
            IpHeaderCheckSum();  // refresh IP header checksum
            checkSumPseudoHeader( udpLength.all ); // get the UDP pseudo-header checksum
            ppp.udp->checksumR = 0; // before TCP checksum calculations the checksum bytes must be set cleared
            unsigned int pseudoHeaderSum=dataCheckSum(ppp.udpStart,udpLength.all, 0); // continue the TCP checksum on the whole TCP packet
            ppp.udp->checksumR = __REV16( pseudoHeaderSum); // tcp checksum done, store it in the TCP header
            sendPppFrame(); // send the UDP message back
        } else if ( testFound ) {
            unsigned int sI = __REV( ppp.ip->srcAdrR );
            unsigned int dI = __REV( ppp.ip->dstAdrR );
            unsigned int sp = __REV16( ppp.udp->srcPortR );
            unsigned int dp = __REV16( ppp.udp->dstPortR );
            int n=sprintf(ppp.pkt.buf+200,"Response Count %d\n", ppp.responseCounter);
            sendUdp(dI,sI,dp,sp,ppp.pkt.buf+200,n); // build a udp packet from the ground up
        }
    }
}

/// handle a PING ICMP (internet control message protocol) packet
void ICMPpacket()   // internet control message protocol
{
    struct {
        unsigned int all; // length of entire ip packet
        unsigned int header; // length of ip header
    } ipLength;
    struct {
        unsigned int all; // length of entire udp packet
        unsigned int data; // length of udp data segment
    } icmpLength;
    ipLength.all = __REV16( ppp.ip->lengthR );  // length of ip packet
    ipLength.header = 4 * ppp.ip->headerLength; // length of ip header
    ppp.icmpStart = ppp.ipStart + ipLength.header; // calculate start of udp header
    icmpLength.all = ipLength.all - ipLength.header; // length of icmp packet
    icmpLength.data = icmpLength.all - 8; // length of icmp data
#define ICMP_TYPE_PING_REQUEST 8
    if ( ppp.icmp->type == ICMP_TYPE_PING_REQUEST ) {
        ppp.ip->ttl--; // decrement time to live (so we have to update header checksum)
        swapIpAddresses(); // swap the IP source and destination addresses
        IpHeaderCheckSum();  // new ip header checksum (required because we changed TTL)
#define ICMP_TYPE_ECHO_REPLY 0
        ppp.icmp->type = ICMP_TYPE_ECHO_REPLY; // icmp echo reply
        ppp.icmp->checkSumR = 0; // zero the checksum for recalculation
        unsigned int sum = dataCheckSum(ppp.icmpStart, icmpLength.all, 1); // icmp checksum
        ppp.icmp->checkSumR = __REV16( sum ); // save big-endian icmp checksum

        int printSize = icmpLength.data; // exclude size of icmp header
        if (printSize > 10) printSize = 10; // print up to 20 characters

        sendPppFrame(); // reply to the ping
    }
}

/// Encode a buffer in base-64
const static char lut [] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
void enc64(char * in, char * out, int len)
{
    int i,j,a,b,c;
    i=0;
    j=0;
    while(1) {
        if (i<len) {
            a = in[i++];
            out[j++] = lut[ ( (a >> 2) & 0x3f) ];
        } else break;
        if (i<len) {
            b = in[i++];
            out[j++] = lut[ ( (a << 4) & 0x30) | ( (b >> 4) & 0x0f) ];
            out[j++] = lut[ ( (b << 2) & 0x3c)  ];
        } else out[j++] = '=';
        if (i<len) {
            c = in[i++];
            j--;
            out[j++] = lut[ ( (b << 2) & 0x3c) | ( (c >> 6) & 0x03) ];
            out[j++] = lut[ ( (c >> 0) & 0x3f) ];
        } else out[j++] = '=';
    }
    out[j]=0;
}

#define TCP_FLAG_ACK (1<<4)
#define TCP_FLAG_SYN (1<<1)
#define TCP_FLAG_PSH (1<<3)
#define TCP_FLAG_RST (1<<2)
#define TCP_FLAG_FIN (1<<0)

/// respond to an HTTP request
int httpResponse(char * dataStart, int * flags)
{
    int n=0; // number of bytes we have printed so far

    int nHeader; // byte size of HTTP header
    int contentLengthStart; // index where HTML starts
    int httpGet5, httpGetRoot; // temporary storage of strncmp results
    *flags = TCP_FLAG_ACK | TCP_FLAG_FIN; // the default case is that we close the connection

    httpGetRoot = strncmp(dataStart, "GET /", 5);  // found a GET to the root directory
    httpGet5    = dataStart[5]; // the first character in the path name, we use it for special functions later on

    if((httpGetRoot==0)) {
        n=n+sprintf(n+dataStart,"HTTP/1.1 200 OK\r\nServer: Blinky-Radio\r\n"); // 200 OK header
    } else {
        n=n+sprintf(n+dataStart,"HTTP/1.1 404 Not Found\r\nServer: Blinky-Radio\r\n"); // 404 header
    }
    n=n+sprintf(n+dataStart,"Content-Length: "); // http header
    contentLengthStart = n; // remember where Content-Length is in buffer
    n=n+sprintf(n+dataStart,"?????\r\n"); // leave five spaces for content length - will be updated later
    n=n+sprintf(n+dataStart,"Connection: close\r\n"); // close connection immediately
    n=n+sprintf(n+dataStart,"Content-Type: text/html; charset=us-ascii\r\n\r\n"); // http header must end with empty line (\r\n)
    nHeader=n; // size of HTTP header
    
    if( httpGetRoot == 0 ) {
    	if (httpGet5 == 'a') {
        	// Toggle led
        	static uint8_t ledState1 = 0;

        	ledState1 = ledState1 ? 0 : 1;
        	Genfsk_Send(gCtEvtTxDone_c, NULL, ledState1,1);
        	Led2Toggle();
    	}
    	if (httpGet5 == 'b') {
    	    // Toggle led
    	    static uint8_t ledState2 = 0;

    	    ledState2 = ledState2 ? 0 : 1;
    	    Genfsk_Send(gCtEvtTxDone_c, NULL, ledState2,2);
    	    Led3Toggle();
    	}
    	if (httpGet5 == 'c') {
    	    // Toggle led
    	    static uint8_t ledState3 = 0;

    	    ledState3 = ledState3 ? 0 : 1;
    	    Genfsk_Send(gCtEvtTxDone_c, NULL, ledState3,3);
    	    Led4Toggle();
    	}
        // this is where we insert our web page into the buffer
        memcpy(n+dataStart,rootWebPage,sizeof(rootWebPage));
        n = n + sizeof(rootWebPage)-1; // one less than sizeof because we don't count the null byte at the end

    }

#define CONTENTLENGTHSIZE 5
    char contentLengthString[CONTENTLENGTHSIZE+1];
    snprintf(contentLengthString,CONTENTLENGTHSIZE+1,"%*d",CONTENTLENGTHSIZE,n-nHeader); // print Content-Length with leading spaces and fixed width equal to csize
    memcpy(dataStart+contentLengthStart, contentLengthString, CONTENTLENGTHSIZE); // copy Content-Length to it's place in the send buffer
    return n; // total byte size of our response
}

/// handle an incoming TCP packet
/// use the first few bytes to figure out if it's a websocket, an http request or just pure incoming TCP data
void tcpHandler()
{
    int packetLengthIp = __REV16(ppp.ip->lengthR ); // size of ip packet
    int headerSizeIp = 4 * ppp.ip->headerLength;  // size of ip header
    ppp.tcpStart = ppp.ipStart + headerSizeIp; // calculate TCP header start
    int tcpSize = packetLengthIp - headerSizeIp; // tcp size = size of ip payload
    int headerSizeTcp = 4 * (ppp.tcp->offset); // tcp "offset" for start of data is also the header size
    char * tcpDataIn = ppp.tcpStart + headerSizeTcp; // start of data TCP data after TCP header
    int tcpDataSize = tcpSize - headerSizeTcp; // size of data block after TCP header

    unsigned int seq_in = __REV(ppp.tcp->seqTcpR);
    unsigned int ack_in = __REV(ppp.tcp->ackTcpR);
    unsigned int ack_out = seq_in + tcpDataSize;
    unsigned int seq_out = ack_in; // use their version of our current sequence number

    // first we shorten the TCP response header to only 20 bytes. This means we ignore all TCP option requests
    headerSizeIp=20;
    ppp.ip->headerLength = headerSizeIp/4; // ip header is 20 bytes long
    ppp.ip->lengthR = __REV(40); // 20 ip header + 20 tcp header
    headerSizeTcp = 20; // shorten outgoing TCP header size 20 bytes
    ppp.tcpStart = ppp.ipStart + headerSizeIp; // recalc TCP header start
    ppp.tcp->offset = (headerSizeTcp/4);
    char * tcpDataOut = ppp.tcpStart + headerSizeTcp; // start of outgoing data

    int dataLen = 0; // most of our responses will have zero TCP data, only a header
    int flagsOut = TCP_FLAG_ACK; // the default case is an ACK packet

    ppp.tcp->windowR = __REV16( 1200 ); // set tcp window size to 1200 bytes

    // A sparse TCP flag interpreter that implements stateless TCP connections

    switch ( ppp.tcp->flag.All ) {
        case TCP_FLAG_SYN:
            flagsOut = TCP_FLAG_SYN | TCP_FLAG_ACK; // something wants to connect - acknowledge it
            seq_out = seq_in+0x10000000U; // create a new sequence number using their sequence as a starting point, increase the highest digit
            ack_out++; // for SYN flag we have to increase the sequence by 1
            break;
        case TCP_FLAG_ACK:
        case TCP_FLAG_ACK | TCP_FLAG_PSH:
            if ( (ppp.tcp->flag.All == TCP_FLAG_ACK) && (tcpDataSize == 0)) return; // handle zero-size ack messages by ignoring them
            if ( (strncmp(tcpDataIn, "GET /", 5) == 0) ) { // check for an http GET command
                flagsOut = TCP_FLAG_ACK | TCP_FLAG_PSH; // we have data, set the PSH flag
                dataLen = httpResponse(tcpDataOut, &flagsOut); // send an http response
            }
            break;
        case TCP_FLAG_FIN:
        case TCP_FLAG_FIN | TCP_FLAG_ACK:
        case TCP_FLAG_FIN | TCP_FLAG_PSH | TCP_FLAG_ACK:
            flagsOut = TCP_FLAG_ACK | TCP_FLAG_FIN; // set outgoing FIN flag to ask them to close from their side
            ack_out++; // for FIN flag we have to increase the sequence by 1
            break;
        default:
            return; // ignore all other packets
    } // switch

    // The TCP flag handling is now done
    // first we swap source and destination TCP addresses and insert the new ack and seq numbers
    swapIpAddresses(); // swap IP source and destination addresses
    swapIpPorts(); // swap IP  source and destination ports

    ppp.tcp->ackTcpR = __REV( ack_out ); // byte reversed - tcp/ip messages are big-endian (high byte first)
    ppp.tcp->seqTcpR = __REV( seq_out ); // byte reversed - tcp/ip messages are big-endian (high byte first)

    ppp.tcp->flag.All = flagsOut; // update the TCP flags

    // recalculate all the header sizes
    tcpSize = headerSizeTcp + dataLen; // tcp packet size
    int newPacketSize = headerSizeIp + tcpSize; // calculate size of the outgoing packet
    ppp.ip->lengthR = __REV16 ( newPacketSize );
    ppp.pkt.len = newPacketSize+4+2; // ip packet length + 4-byte ppp prefix (ff 03 00 21) + 2 fcs (crc) bytes bytes at the end of the packet

    // the header is all set up, now do the IP and TCP checksums
    IpHeaderCheckSum(); // calculate new IP header checksum
    checkSumPseudoHeader( tcpSize ); // get the TCP pseudo-header checksum
    ppp.tcp->checksumR = 0; // before TCP checksum calculations the checksum bytes must be set cleared
    unsigned int pseudoHeaderSum=dataCheckSum(ppp.tcpStart,tcpSize, 0); // continue the TCP checksum on the whole TCP packet
    ppp.tcp->checksumR = __REV16( pseudoHeaderSum); // tcp checksum done, store it in the TCP header

    sendPppFrame(); // All preparation complete - send the TCP response

    memset(ppp.pkt.buf+44,0,500); // flush out traces of previous data that we may scan for
}

/// process an incoming IP packet
void IPframe()
{
    int protocol = ppp.ip->protocol;
    switch (protocol) {
        case    1:
            ICMPpacket();
            break;
        case   17:
            UDPpacket();
            break;
        case    6:
            tcpHandler();
            break;
        default:
            break;
    }
}

/// respond to LCP (line configuration protocol) configuration request) by allowing no options
void LCPconfReq()
{
    if ( ppp.lcp->lengthR != __REV16(4) ) {
        ppp.lcp->code=4; // allow only "no options" which means Maximum Receive Unit (MRU) is default 1500 bytes
        sendPppFrame();
    } else {
        ppp.lcp->code=2; // ack zero conf
        sendPppFrame();
        ppp.lcp->code=1; // request no options
        sendPppFrame();
    }
}

/// handle LCP end (disconnect) packets by acknowledging them and by setting ppp.online to false
void LCPend()
{
    ppp.lcp->code=6; // end
    sendPppFrame(); // acknowledge
    ppp.online=0; // start hunting for connect string again
    pppInitStruct(); // flush the receive buffer
}

/// process incoming LCP packets
void LCPframe()
{
    int code = ppp.lcp->code;
    switch (code) {
        case 1:
            LCPconfReq();
            break; // config request
        case 5:
            LCPend();
            break; // end connection
        default:
            break;
    }
}

/// determine the packet type (IP, IPCP or LCP) of incoming packets
void determinePacketType()
{
    if ( ppp.ppp->address != 0xff ) {
        return;
    }
    if ( ppp.ppp->control != 3 ) {
        return;
    }
    unsigned int protocol = __REV16( ppp.ppp->protocolR );
    switch ( protocol ) {
        case 0xc021:
            LCPframe();
            break;  // link control
        case 0x8021:
            IPCPframe();
            break;  // IP control
        case 0x0021:
            IPframe();
            break;  // IP itself
        default:
            break;
    }
}

/// PPP serial port receive interrupt handler.
/// Check for available characters from the PC and read them into our own circular serial receive buffer at ppp.rx.buf.
/// Also, if we are offline and a 0x7e frame start character is seen, we go online immediately
void pppReceiveHandler()
{
    char ch;
    uint16_t count;

    while (1) {
    	Serial_Read(pc, (uint8_t*)&ch, 1, &count);

    	if (count < 1) {
    		break;
    	}

        int hd = (ppp.rx.head+1)&(RXBUFLEN-1); // increment/wrap head index
        if ( hd == ppp.rx.rtail ) {
            return;
        }

        ppp.rx.buf[ppp.rx.head] = ch; // insert in our receive buffer
        if ( ppp.online == 0 ) {
            if (ch == 0x7E) {
                ppp.online = 1;
            }
        }
        ppp.rx.head = hd; // update head pointer
        ppp.rx.buflevel++;
    }

    bool_t hasData;
    do {
    	hasData = false;
    	if ( ppp.rx.head != ppp.rx.tail ) {
            	hasData = true;
                int oldTail = ppp.rx.tail; // remember where the character is located in the buffer
                int rx = pc_getBuf(); // get the character
                if (rx==FRAME_7E) {
                    if (ppp.firstFrame) { // is this the start of the first frame start
                        ppp.rx.rtail = ppp.rx.tail; // update real-time tail with the virtual tail
                        ppp.hdlc.frameStartIndex = ppp.rx.tail; // remember where first frame started
                        ppp.firstFrame=0; // clear first frame flag
                    }  else {
                        ppp.hdlc.frameEndIndex=oldTail; // mark the frame end character
                        processPPPFrame(ppp.hdlc.frameStartIndex, ppp.hdlc.frameEndIndex); // process the frame
                        ppp.rx.rtail = ppp.rx.tail; // update real-time tail with the virtual tail
                        ppp.hdlc.frameStartIndex = ppp.rx.tail; // remember where next frame started
                        hasData = false;
                    }
                }
            }
    } while (hasData);
}

/// Wait for a dial-up modem connect command ("CLIENT") from the host PC, if found, we set ppp.online to true, which will start the IP packet scanner.
void waitForPcConnectString()
{
    while(ppp.online == 0) {
        // search for Windows Dialup Networking "Direct Connection Between Two Computers" expected connect string
        char * found1 = strstr( (char *)ppp.rx.buf, "CLIENT" );
        if (found1 != NULL) {
            // respond with Windows Dialup networking expected "Direct Connection Between Two Computers" response string
            Serial_Print(pc, "CLIENTSERVER", gNoBlock_d);
            ppp.online=1; // we are connected - set flag so we stop looking for the connect string
        }
    }
}

/// Initialize PPP data structure and set serial port(s) baud rate(s)
void initializePpp(uint8_t serial)
{
	pc = serial;
    pppInitStruct(); // initialize all the variables/properties/buffers
    Serial_Print(pc, "Initialized PPP", gAllowToBlock_d);
}
