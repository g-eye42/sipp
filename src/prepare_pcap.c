/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Author : Guillaume TEISSIER from FTR&D 02/02/2006
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pcap.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#if defined(__HPUX) || defined(__CYGWIN) || defined(__FreeBSD__)
#include <netinet/in_systm.h>
#endif
#include <netinet/ip.h>
#ifndef __CYGWIN
#include <netinet/ip6.h>
#endif
#include <string.h>

#include "prepare_pcap.h"
#include "screen.hpp"

/* We define our own structures for Ethernet Header and IPv6 Header as they are not available on CYGWIN.
 * We only need the fields, which are necessary to determine the type of the next header.
 * we could also define our own structures for UDP and IPv4. We currently use the structures
 * made available by the platform, as we had no problems to get them on all supported platforms.
 */

typedef struct _ether_type_hdr {
    uint16_t ether_type; /* we only need the type, so we can determine, if the next header is IPv4 or IPv6 */
} ether_type_hdr;

typedef struct _ipv6_hdr {
    char dontcare[6];
    uint8_t nxt_header; /* we only need the next header, so we can determine, if the next header is UDP or not */
    char dontcare2[33];
} ipv6_hdr;


#ifdef __HPUX
int check(uint16_t *buffer, int len)
{
#else
inline int check(uint16_t *buffer, int len)
{
#endif
    int sum;
    int i;
    sum = 0;

    for (i=0; i<(len&~1); i+= 2)
        sum += *buffer++;

    if (len & 1) {
        sum += htons( (*(const uint8_t *)buffer) << 8);
    }
    return sum;
}

#ifdef __HPUX
uint16_t checksum_carry(int s)
{
#else
inline uint16_t checksum_carry(int s)
{
#endif
    int s_c = (s >> 16) + (s & 0xffff);
    return (~(s_c + (s_c >> 16)) & 0xffff);
}

char errbuf[PCAP_ERRBUF_SIZE];

/* prepare a pcap file
 */
int prepare_pkts(char *file, pcap_pkts *pkts)
{
    pcap_t *pcap;
    struct pcap_pkthdr *pkthdr = NULL;
    uint8_t *pktdata = NULL;
    int n_pkts = 0;
    uint64_t max_length = 0;
    uint16_t base = 0xffff;
    uint64_t pktlen;
    pcap_pkt *pkt_index;
    size_t ether_type_offset;
    ether_type_hdr *ethhdr;

    struct iphdr *iphdr;
    ipv6_hdr *ip6hdr;
    struct udphdr *udphdr;

    pkts->pkts = NULL;

    pcap = pcap_open_offline(file, errbuf);
    if (!pcap)
        ERROR_NO("Can't open PCAP file '%s'", file);

    switch (pcap_datalink(pcap)) {
    case DLT_EN10MB:
        /* srcmac[6], dstmac[6], ethertype[2] */
        ether_type_offset = 12;
        break;
    case DLT_LINUX_SLL:
        /* some_stuff[14], ethertype[2] */
        ether_type_offset = 14;
        break;
    default:
        ERROR("Unsupported link-type %d", pcap_datalink(pcap));
    }

#if HAVE_PCAP_NEXT_EX
    while (pcap_next_ex (pcap, &pkthdr, (const uint8_t **) &pktdata) == 1) {
#else
#ifdef __HPUX
    pkthdr = (pcap_pkthdr *) malloc (sizeof (*pkthdr));
#else
    pkthdr = malloc (sizeof (*pkthdr));
#endif
    if (!pkthdr)
        ERROR("Can't allocate memory for pcap pkthdr");
    while ((pktdata = (uint8_t *) pcap_next (pcap, pkthdr)) != NULL) {
#endif
        if (pkthdr->len != pkthdr->caplen) {
            ERROR("You got truncated packets. Please create a new dump with -s0");
        }
        ethhdr = (ether_type_hdr *)(pktdata + ether_type_offset);
        if (ntohs(ethhdr->ether_type) != 0x0800 /* IPv4 */
                && ntohs(ethhdr->ether_type) != 0x86dd) { /* IPv6 */
            fprintf(stderr, "Ignoring non IP{4,6} packet, got ether_type %hu!\n",
                    ntohs(ethhdr->ether_type));
            continue;
        }
        iphdr = (struct iphdr *)((char *)ethhdr + sizeof(*ethhdr));
        if (iphdr && iphdr->version == 6) {
            //ipv6
            pktlen = (uint64_t) pkthdr->len - sizeof(*ethhdr) - sizeof(*ip6hdr);
            ip6hdr = (ipv6_hdr *)(void *) iphdr;
            if (ip6hdr->nxt_header != IPPROTO_UDP) {
                fprintf(stderr, "prepare_pcap.c: Ignoring non UDP packet!\n");
                continue;
            }
            udphdr = (struct udphdr *)((char *)ip6hdr + sizeof(*ip6hdr));
        } else {
            //ipv4
            if (iphdr->protocol != IPPROTO_UDP) {
                fprintf(stderr, "prepare_pcap.c: Ignoring non UDP packet!\n");
                continue;
            }
#if defined(__DARWIN) || defined(__CYGWIN) || defined(__FreeBSD__)
            udphdr = (struct udphdr *)((char *)iphdr + (iphdr->ihl << 2) + 4);
            pktlen = (uint64_t)(ntohs(udphdr->uh_ulen));
#elif defined ( __HPUX)
            udphdr = (struct udphdr *)((char *)iphdr + (iphdr->ihl << 2));
            pktlen = (uint64_t) pkthdr->len - sizeof(*ethhdr) - sizeof(*iphdr);
#else
            udphdr = (struct udphdr *)((char *)iphdr + (iphdr->ihl << 2));
            pktlen = (uint64_t)(ntohs(udphdr->len));
#endif
        }
        if (pktlen > PCAP_MAXPACKET) {
            ERROR("Packet size is too big! Recompile with bigger PCAP_MAXPACKET in prepare_pcap.h");
        }
        pkts->pkts = (pcap_pkt *) realloc(pkts->pkts, sizeof(*(pkts->pkts)) * (n_pkts + 1));
        if (!pkts->pkts)
            ERROR("Can't re-allocate memory for pcap pkt");
        pkt_index = pkts->pkts + n_pkts;
        pkt_index->pktlen = pktlen;
        pkt_index->ts = pkthdr->ts;
        pkt_index->data = (unsigned char *) malloc(pktlen);
        if (!pkt_index->data)
            ERROR("Can't allocate memory for pcap pkt data");
        memcpy(pkt_index->data, udphdr, pktlen);

#if defined(__HPUX) || defined(__DARWIN) || (defined __CYGWIN) || defined(__FreeBSD__)
        udphdr->uh_sum = 0 ;
#else
        udphdr->check = 0;
#endif

        // compute a partial udp checksum
        // not including port that will be changed
        // when sending RTP
#if defined(__HPUX) || defined(__DARWIN) || (defined __CYGWIN) || defined(__FreeBSD__)
        pkt_index->partial_check = check((uint16_t *) &udphdr->uh_ulen, pktlen - 4) + ntohs(IPPROTO_UDP + pktlen);
#else
        pkt_index->partial_check = check((uint16_t *) &udphdr->len, pktlen - 4) + ntohs(IPPROTO_UDP + pktlen);
#endif
        if (max_length < pktlen)
            max_length = pktlen;
#if defined(__HPUX) || defined(__DARWIN) || (defined __CYGWIN) || defined(__FreeBSD__)
        if (base > ntohs(udphdr->uh_dport))
            base = ntohs(udphdr->uh_dport);
#else
        if (base > ntohs(udphdr->dest))
            base = ntohs(udphdr->dest);
#endif
        n_pkts++;
    }
    pkts->max = pkts->pkts + n_pkts;
    pkts->max_length = max_length;
    pkts->base = base;
    fprintf(stderr, "In pcap %s, npkts %d\nmax pkt length %ld\nbase port %d\n", file, n_pkts, max_length, base);
    pcap_close(pcap);

#ifndef HAVE_PCAP_NEXT_EX
    free(pkthdr);
#endif

    return 0;
}
