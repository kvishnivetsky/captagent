/*
 * $Id$
 *
 *  captagent - Homer capture agent. Modular
 *  Duplicate SIP messages in Homer Encapulate Protocol [HEP] [ipv6 version]
 *
 *  Author: Alexandr Dubovikov <alexandr.dubovikov@gmail.com>
 *  (C) Homer Project 2012-2014 (http://www.sipcapture.org)
 *
 * Homer capture agent is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version
 *
 * Homer capture agent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <ctype.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#ifndef __FAVOR_BSD
#define __FAVOR_BSD
#endif /* __FAVOR_BSD */

#include <netinet/ip.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#ifdef USE_IPV6
#include <netinet/ip6.h>
#endif /* USE_IPV6 */

#include <pcap.h>

#include "../../src/api.h"
#include "../../src/log.h"
#include "proto_rtcp.h"
#include "../proto_uni/capthash.h"
#include "rtcp_parser.h"

static uint8_t rtcp_link_offset = 14;
uint8_t hdr_offset = 0;

pcap_t *sniffer_rtp;
pthread_t rtp_thread;   

unsigned char* ethaddr = NULL;
unsigned char* mplsaddr = NULL;

/* Callback function that is passed to pcap_loop() */ 
void rtcpback_proto(u_char *useless, struct pcap_pkthdr *pkthdr, u_char *packet) 
{

	/* Pat Callahan's patch for MPLS */
	memcpy(&ethaddr, (packet + 12), 2);
        memcpy(&mplsaddr, (packet + 16), 2);

        if (ntohs((uint16_t)*(&ethaddr)) == 0x8100) {
          if (ntohs((uint16_t)*(&mplsaddr)) == 0x8847) {
             hdr_offset = 8;
          } else {
             hdr_offset = 4;
          }
        }

        struct ip      *ip4_pkt = (struct ip *)    (packet + rtcp_link_offset + hdr_offset);
#if USE_IPv6
        struct ip6_hdr *ip6_pkt = (struct ip6_hdr*)(packet + rtcp_link_offset + ((ntohs((uint16_t)*(packet + 12)) == 0x8100)? 4: 0) );
#endif

	uint32_t ip_ver;
	uint8_t  ip_proto = 0;
	uint32_t ip_hl    = 0;
	uint32_t ip_off   = 0;
	uint8_t  fragmented  = 0;
	uint16_t frag_offset = 0;
	uint32_t frag_id     = 0;
	char ip_src[INET6_ADDRSTRLEN + 1],
		ip_dst[INET6_ADDRSTRLEN + 1];

        unsigned char *data;
	    
	uint32_t len = pkthdr->caplen;

	ip_ver = ip4_pkt->ip_v;

	switch (ip_ver) {

	        case 4: {
#if defined(AIX)
#undef ip_hl
        	    ip_hl       = ip4_pkt->ip_ff.ip_fhl * 4;
#else
	            ip_hl       = ip4_pkt->ip_hl * 4;
#endif
        	    ip_proto    = ip4_pkt->ip_p;
	            ip_off      = ntohs(ip4_pkt->ip_off);

        	    fragmented  = ip_off & (IP_MF | IP_OFFMASK);
	            frag_offset = (fragmented) ? (ip_off & IP_OFFMASK) * 8 : 0;
        	    frag_id     = ntohs(ip4_pkt->ip_id);

	            inet_ntop(AF_INET, (const void *)&ip4_pkt->ip_src, ip_src, sizeof(ip_src));
	            inet_ntop(AF_INET, (const void *)&ip4_pkt->ip_dst, ip_dst, sizeof(ip_dst));
        	} break;

#if USE_IPv6
	        case 6: {
        	    ip_hl    = sizeof(struct ip6_hdr);
	            ip_proto = ip6_pkt->ip6_nxt;

        	    if (ip_proto == IPPROTO_FRAGMENT) {
                	struct ip6_frag *ip6_fraghdr;

	                ip6_fraghdr = (struct ip6_frag *)((unsigned char *)(ip6_pkt) + ip_hl);
        	        ip_hl      += sizeof(struct ip6_frag);
	                ip_proto    = ip6_fraghdr->ip6f_nxt;

        	        fragmented  = 1;
                	frag_offset = ntohs(ip6_fraghdr->ip6f_offlg & IP6F_OFF_MASK);
	                frag_id     = ntohl(ip6_fraghdr->ip6f_ident);
        	    }

	            inet_ntop(AF_INET6, (const void *)&ip6_pkt->ip6_src, ip_src, sizeof(ip_src));
        	    inet_ntop(AF_INET6, (const void *)&ip6_pkt->ip6_dst, ip_dst, sizeof(ip_dst));
	        } break;
#endif
	}       

	switch (ip_proto) {

                case IPPROTO_UDP: {
                    struct udphdr *udp_pkt = (struct udphdr *)((unsigned char *)(ip4_pkt) + ip_hl);
                    uint16_t udphdr_offset = (frag_offset) ? 0 : sizeof(*udp_pkt);

                    data = (unsigned char *)(udp_pkt) + udphdr_offset;
                    
                    len -= rtcp_link_offset + ip_hl + udphdr_offset + hdr_offset;

#if USE_IPv6
                    if (ip_ver == 6)
                        len -= ntohs(ip6_pkt->ip6_plen);
#endif

                    if ((int32_t)len < 0) len = 0;

                      dump_rtp_packet(pkthdr, packet, ip_proto, data, len, ip_src, ip_dst,
                        ntohs(udp_pkt->uh_sport), ntohs(udp_pkt->uh_dport), 0,
                        udphdr_offset, fragmented, frag_offset, frag_id, ip_ver);
                                           
                                                
                } break;

                default:                 
                        break;
        }        
}


int dump_rtp_packet(struct pcap_pkthdr *pkthdr, u_char *packet, uint8_t proto, unsigned char *data, uint32_t len,
                 const char *ip_src, const char *ip_dst, uint16_t sport, uint16_t dport, uint8_t flags,
                                  uint16_t hdr_offset, uint8_t frag, uint16_t frag_offset, uint32_t frag_id, uint32_t ip_ver) {

        struct timeval tv;
        time_t curtime;
	char timebuffer[30];	
	rc_info_t *rcinfo = NULL;
        unsigned char *senddata;	        
        int json_len;
        
        gettimeofday(&tv,NULL);

        sendPacketsCount++;

        curtime = tv.tv_sec;
        strftime(timebuffer,30,"%m-%d-%Y  %T.",localtime(&curtime));

        if(len < 5) {
             LERR("rtcp the message is too small: %d\n", len);
             return -1;
        }

        LDEBUG("GOT RTCP %s:%d -> %s:%d. LEN: %d\n", ip_src, sport, ip_dst, dport, len);

        if(find_and_update(sip_callid, ip_src, sport, ip_dst, dport) == 0) {

            return 0;
        }
	
	if(rtcp_as_json) {		
            json_rtcp_buffer[0] = '\0';	
      	    if((json_len = capt_parse_rtcp((char *)data, len, json_rtcp_buffer, sizeof(json_rtcp_buffer))) > 0) {
      	          senddata = json_rtcp_buffer;
      	          len = strlen(json_rtcp_buffer);
      	    }
      	    else {
      	    	LDEBUG("GOODBYE or APP MESSAGE. Ignore!\n");
      	    	return 0;
      	    }
      	    
      	    LDEBUG("JSON RTCP %s\n", json_rtcp_buffer);
        }
        else senddata = data;

	rcinfo = malloc(sizeof(rc_info_t));
	memset(rcinfo, 0, sizeof(rc_info_t));
	
	LDEBUG("CALLID RTCP %s\n", sip_callid);

        rcinfo->src_port   = sport;
        rcinfo->dst_port   = dport;
        rcinfo->src_ip     = ip_src;
        rcinfo->dst_ip     = ip_dst;
        rcinfo->ip_family  = ip_ver = 4 ? AF_INET : AF_INET6 ;
        rcinfo->ip_proto   = proto;
        rcinfo->time_sec   = pkthdr->ts.tv_sec;
        rcinfo->time_usec  = pkthdr->ts.tv_usec;
        rcinfo->proto_type = rtcp_proto_type;
        /* correlation stuff */
        rcinfo->correlation_id.len = strlen(sip_callid);
        rcinfo->correlation_id.s = &sip_callid;
                        
        if(debug_proto_rtcp_enable)
            LDEBUG("SENDING PACKET: Len: [%d]\n", len);

	/* Duplcate */
	if(!send_message(rcinfo, senddata, (unsigned int) len)) {
	         LERR("Not duplicated\n");
        }        
        
        if(rcinfo) free(rcinfo);

	return 1;
}

void* rtp_collect( void* device ) {

        struct bpf_program filter;
        char errbuf[PCAP_ERRBUF_SIZE];
        char *filter_expr;
        uint16_t snaplen = 65535, timeout = 100, len = 300, ret = 0;        

        if(device) {
            if((sniffer_rtp = pcap_open_live((char *)device, snaplen, rtcp_promisc, timeout, errbuf)) == NULL) {
                LERR("Failed to open packet sniffer on %s: pcap_open_live(): %s\n", (char *)device, errbuf);
                return NULL;
            }
        } else  {
            if((sniffer_rtp = pcap_open_offline(usefile, errbuf)) == NULL) {
                LERR("Failed to open packet sniffer rtp on %s: pcap_open_offline(): %s\n", usefile, errbuf);
                return NULL;
            }
        }

        len += (rtcp_portrange != NULL) ? strlen(rtcp_portrange) : 10;        
        len += (rtcp_userfilter != NULL) ? strlen(rtcp_userfilter) : 0;        
        filter_expr = malloc(sizeof(char) * len);
        
        ret += snprintf(filter_expr, len, RTCP_FILTER);
                        
        /* FILTER */
        if(rtcp_portrange != NULL) ret += snprintf(filter_expr+ret, (len - ret), "%s portrange %s ", ret ? " and": "", rtcp_portrange);

        /* CUSTOM FILTER */
        if(rtcp_userfilter != NULL) ret += snprintf(filter_expr+ret, (len - ret), " %s", rtcp_userfilter);

        /* compile filter expression (global constant, see above) */
        if (pcap_compile(sniffer_rtp, &filter, filter_expr, 1, 0) == -1) {
                LERR("Failed to compile filter \"%s\": %s\n", filter_expr, pcap_geterr(sniffer_rtp));
                if(filter_expr) free(filter_expr);
                return NULL;
        }

        /* install filter on sniffer session */
        if (pcap_setfilter(sniffer_rtp, &filter)) {
                LERR("Failed to install filter: %s\n", pcap_geterr(sniffer_rtp));
                if(filter_expr) free(filter_expr);
                return NULL;
        }

        if(filter_expr) free(filter_expr);
        
        /* detect rtcp_link_offset. Thanks ngrep for this. */
        switch(pcap_datalink(sniffer_rtp)) {
                case DLT_EN10MB:
                    rtcp_link_offset = ETHHDR_SIZE;
                    break;

                case DLT_IEEE802:
                    rtcp_link_offset = TOKENRING_SIZE;
                    break;

                case DLT_FDDI:
                    rtcp_link_offset = FDDIHDR_SIZE;
                    break;

                case DLT_SLIP:
                    rtcp_link_offset = SLIPHDR_SIZE;
                    break;

                case DLT_PPP:
                    rtcp_link_offset = PPPHDR_SIZE;
                    break;

                case DLT_LOOP:
                case DLT_NULL:
                    rtcp_link_offset = LOOPHDR_SIZE;
                    break;

                case DLT_RAW:
                    rtcp_link_offset = RAWHDR_SIZE;
                    break;

                case DLT_LINUX_SLL:
                    rtcp_link_offset = ISDNHDR_SIZE;
                    break;

                case DLT_IEEE802_11:
                    rtcp_link_offset = IEEE80211HDR_SIZE;
                    break;

                default:
                    LERR( "fatal: unsupported interface type %u\n", pcap_datalink(sniffer_rtp));
                    exit(-1);
        }

        while (pcap_loop(sniffer_rtp, 0, (pcap_handler)rtcpback_proto, 0));


        /* terminate from here */
        handler(1);

        return NULL;
}




int unload_module(void)
{
        LNOTICE("unloaded module proto_rtcp\n");

	 /* Close socket */
        pcap_close(sniffer_rtp);        

        return 0;
}

int load_module(xml_node *config)
{
        char *dev = NULL, *usedev = NULL;
        char errbuf[PCAP_ERRBUF_SIZE];                                
        xml_node *modules;
        char *key, *value = NULL;
        
        LNOTICE("Loaded proto_rtcp\n");
                                           
        /* READ CONFIG */
        modules = config;

        while(1) {
                if(modules ==  NULL) break;
                modules = xml_get("param", modules, 1 );
                if(modules->attr[0] != NULL && modules->attr[2] != NULL) {

                        /* bad parser */
                        if(strncmp(modules->attr[2], "value", 5) || strncmp(modules->attr[0], "name", 4)) {
                            LERR( "bad keys in the config\n");
                            goto next;
                        }

                        key =  modules->attr[1];
                        value = modules->attr[3];

                        if(key == NULL || value == NULL) {
                            LERR( "bad values in the config\n");
                            goto next;

                        }

                        if(!strncmp(key, "dev", 3)) usedev = value;                        
                        else if(!strncmp(key, "portrange", 9)) rtcp_portrange = value;
                        else if(!strncmp(key, "promisc", 7) && !strncmp(value, "false", 5)) rtcp_promisc = 0;
                        else if(!strncmp(key, "filter", 6)) rtcp_userfilter = value;
                        else if(!strncmp(key, "rtcp-json", 9) && !strncmp(value, "false", 5) ) rtcp_as_json = 0;
                        else if(!strncmp(key, "send-sdes", 9) && !strncmp(value, "false", 5) ) send_sdes = 0;
                        else if(!strncmp(key, "vlan", 4) && !strncmp(value, "true", 4)) rtcp_vlan = 1;
                        else if(!strncmp(key, "debug", 5) && !strncmp(value, "true", 4)) debug_proto_rtcp_enable = 1;
                }
next:

                modules = modules->next;
        }

        /* DEV || FILE */
        if(!usefile) {
          dev = usedev ? usedev : pcap_lookupdev(errbuf);
          if (!dev) {
              perror(errbuf);
              exit(-1);
          }
        }

        // start thread
        pthread_create(&rtp_thread, NULL, rtp_collect, (void *)dev);
        
                                         
        return 0;
}


char *description(void)
{
        LNOTICE("Loaded description\n");
        char *description = "test description";
        
        return description;
}


int statistic(char *buf)
{
        snprintf(buf, 1024, "Statistic of PROTO_RTCP module:\r\nSend packets: [%i]\r\n", sendPacketsCount);
        return 1;
}
                        
