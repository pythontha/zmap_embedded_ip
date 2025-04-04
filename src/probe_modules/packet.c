/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "../../lib/includes.h"
#include "../../lib/xalloc.h"
#include "packet.h"

#include "module_tcp_synscan.h"
#include "logger.h"

#ifndef NDEBUG
void print_macaddr(struct ifreq *i)
{
	printf("Device %s -> Ethernet %02x:%02x:%02x:%02x:%02x:%02x\n",
	       i->ifr_name, (int)((unsigned char *)&i->ifr_addr.sa_data)[0],
	       (int)((unsigned char *)&i->ifr_addr.sa_data)[1],
	       (int)((unsigned char *)&i->ifr_addr.sa_data)[2],
	       (int)((unsigned char *)&i->ifr_addr.sa_data)[3],
	       (int)((unsigned char *)&i->ifr_addr.sa_data)[4],
	       (int)((unsigned char *)&i->ifr_addr.sa_data)[5]);
}
#endif /* NDEBUG */

#define IP_ADDR_LEN_STR 20

void fprintf_ip_header(FILE *fp, struct ip *iph)
{
	struct in_addr *s = (struct in_addr *)&(iph->ip_src);
	struct in_addr *d = (struct in_addr *)&(iph->ip_dst);

	char srcip[IP_ADDR_LEN_STR + 1];
	char dstip[IP_ADDR_LEN_STR + 1];
	// inet_ntoa is a const char * so we if just call it in
	// fprintf, you'll get back wrong results since we're
	// calling it twice.
	strncpy(srcip, inet_ntoa(*s), IP_ADDR_LEN_STR - 1);
	strncpy(dstip, inet_ntoa(*d), IP_ADDR_LEN_STR - 1);

	srcip[IP_ADDR_LEN_STR] = '\0';
	dstip[IP_ADDR_LEN_STR] = '\0';

	fprintf(fp, "ip { saddr: %s | daddr: %s | checksum: %#04X }\n", srcip,
		dstip, ntohs(iph->ip_sum));
}

void fprintf_ipv6_header(FILE *fp, struct ip6_hdr *iph)
{
	struct in6_addr *s = (struct in6_addr *) &(iph->ip6_src);
	struct in6_addr *d = (struct in6_addr *) &(iph->ip6_dst);

	char srcip[INET6_ADDRSTRLEN+1];
	char dstip[INET6_ADDRSTRLEN+1];
	unsigned char next = (unsigned char) (iph->ip6_nxt);

	// TODO: Is restrict correct here?
	inet_ntop(AF_INET6, s, (char * restrict) &srcip, INET6_ADDRSTRLEN);
	inet_ntop(AF_INET6, d, (char * restrict) &dstip, INET6_ADDRSTRLEN);

	srcip[INET6_ADDRSTRLEN] = '\0';
	dstip[INET6_ADDRSTRLEN] = '\0';

	fprintf(fp, "ip6 { saddr: %s | daddr: %s | nxthdr: %u }\n",
			srcip,
			dstip,
			next);
}

void fprintf_eth_header(FILE *fp, struct ether_header *ethh)
{
	if (!zconf.send_ip_pkts) {
		fprintf(fp,
			"eth { shost: %02x:%02x:%02x:%02x:%02x:%02x | "
			"dhost: %02x:%02x:%02x:%02x:%02x:%02x }\n",
			(int)((unsigned char *)ethh->ether_shost)[0],
			(int)((unsigned char *)ethh->ether_shost)[1],
			(int)((unsigned char *)ethh->ether_shost)[2],
			(int)((unsigned char *)ethh->ether_shost)[3],
			(int)((unsigned char *)ethh->ether_shost)[4],
			(int)((unsigned char *)ethh->ether_shost)[5],
			(int)((unsigned char *)ethh->ether_dhost)[0],
			(int)((unsigned char *)ethh->ether_dhost)[1],
			(int)((unsigned char *)ethh->ether_dhost)[2],
			(int)((unsigned char *)ethh->ether_dhost)[3],
			(int)((unsigned char *)ethh->ether_dhost)[4],
			(int)((unsigned char *)ethh->ether_dhost)[5]);
	}
}

void make_eth_header(struct ether_header *ethh, macaddr_t *src, macaddr_t *dst)
{
	// Create a frame with IPv4 ethertype by default
	make_eth_header_ethertype(ethh, src, dst, ETHERTYPE_IP);
}

void make_eth_header_ethertype(struct ether_header *ethh, macaddr_t *src, macaddr_t *dst, uint16_t ethertype)
{
	memcpy(ethh->ether_shost, src, ETHER_ADDR_LEN);
	memcpy(ethh->ether_dhost, dst, ETHER_ADDR_LEN);
	ethh->ether_type = htons(ethertype);
}

void make_ip_header(struct ip *iph, uint8_t protocol, uint16_t len)
{
	iph->ip_hl = 5;	 // Internet Header Length
	iph->ip_v = 4;	 // IPv4
	iph->ip_tos = 0; // Type of Service
	iph->ip_len = len;
	iph->ip_id = htons(54321); // identification number
	iph->ip_off = 0;	   // fragmentation flag
	iph->ip_ttl = MAXTTL;	   // time to live (TTL)
	iph->ip_p = protocol;	   // upper layer protocol => TCP
	// we set the checksum = 0 for now because that's
	// what it needs to be when we run the IP checksum
	iph->ip_sum = 0;
}

void make_ip6_header(struct ip6_hdr *iph, uint8_t protocol, uint16_t len)
{
	iph->ip6_ctlun.ip6_un2_vfc = 0x60; // 4 bits version, top 4 bits class
	iph->ip6_ctlun.ip6_un1.ip6_un1_plen = htons(len); // payload length
	iph->ip6_ctlun.ip6_un1.ip6_un1_nxt = protocol; // next header
	iph->ip6_ctlun.ip6_un1.ip6_un1_hlim = MAXTTL; // hop limit
}
void make_icmp6_header(struct icmp6_hdr *buf)
{
    buf->icmp6_type = ICMP6_ECHO_REQUEST;
    buf->icmp6_code = 0;
    buf->icmp6_cksum = 0;
    // buf->icmp_seq = 0;
    // TODO: Set ICMP ECHO REQ specific fields
}

void make_icmp_header(struct icmp *buf)
{
	memset(buf, 0, sizeof(struct icmp));
	buf->icmp_type = ICMP_ECHO;
	buf->icmp_code = 0;
	buf->icmp_seq = 0;
}

void make_tcp_header(struct tcphdr *tcp_header, uint16_t th_flags)
{
	tcp_header->th_seq = random();
	tcp_header->th_ack = 0;
	tcp_header->th_x2 = 0;
	tcp_header->th_off = 5; // data offset
	tcp_header->th_flags = 0;
	tcp_header->th_flags |= th_flags;
	tcp_header->th_win = htons(65535); // largest possible window
	tcp_header->th_sum = 0;
	tcp_header->th_urp = 0;
}

size_t set_mss_option(struct tcphdr *tcp_header)
{
	// This only sets MSS, which is a single-word option.
	// seems like assumption here is that word-size = 32 bits
	// MSS field
	// 0 byte = TCP Option Kind = 0x2
	// 1 byte = Length of entire MSS field = 4
	// 2-3 byte = Value of MSS

	size_t header_size = tcp_header->th_off * 4; // 4 is word size
	uint8_t *base = (uint8_t *)tcp_header;
	uint8_t *last_opt = (uint8_t *)base + header_size;

	// TCP Option "header"
	last_opt[0] = 2; // the value in the TCP options spec denoting this as MSS
	last_opt[1] = 4; // MSS is 4 bytes long, length goes here

	// Default Linux MSS is 1460, which 0x05b4
	last_opt[2] = 0x05;
	last_opt[3] = 0xb4;

	tcp_header->th_off += 1;
	return tcp_header->th_off * 4;
}

size_t set_nop_plus_windows_scale(struct tcphdr *tcp_header, uint8_t os)
{
	size_t header_size = tcp_header->th_off * 4;
	uint8_t *last_opt = (uint8_t *)tcp_header + header_size;
	// NOP = 1 byte
	last_opt[0] = 0x01; // kind for NOP
	last_opt += 1;
	// WindowScale = 3 bytes
	last_opt[0] = 0x03; // kind for WindowScale field
	last_opt[1] = 0x03; // length for WindowScale field

	if (os == LINUX_OS_OPTIONS) {
		last_opt[2] = 0x07; // 7 is used as the linux default WindowScale. It represents 2^7 = x128 window size multiplier
	} else if (os == BSD_OS_OPTIONS) {
		last_opt[2] = 0x06; // 6 is used as the MacOS/BSD default WindowScale. It represents 2^6 = x64 window size multiplier
	} else if (os == WINDOWS_OS_OPTIONS) {
		last_opt[2] = 0x08; // 8 is used as the windows default WindowScale. It represents 2^8 = x256 window size multiplier
	}
	tcp_header->th_off += 1;
	return tcp_header->th_off * 4;
}

// sets 2x NOPs and a timestamp (10 bytes)  option = 12 bytes
size_t set_timestamp_option_with_nops(struct tcphdr *tcp_header)
{
	size_t header_size = tcp_header->th_off * 4;
	uint8_t *last_opt = (uint8_t *)tcp_header + header_size;
	// NOP = 1 byte
	last_opt[0] = 0x01; // kind for NOP
	last_opt[1] = 0x01; // kind for NOP
	last_opt += 2;
	// exact method of getting this timestamp isn't important, only that it is a 4 byte value - RFC 7323
	uint32_t now = time(NULL);
	last_opt[0] = 0x08;			  // kind for timestamp field
	last_opt[1] = 0x0a;			  // length for timestamp field
	*(uint32_t *)(last_opt + 2) = htonl(now); // set current time in correct byte order
	// final 4 bytes of timestamp field are left zeroed for the timestamp echo value
	last_opt += 10; // update our pointer 10 bytes ahead
	tcp_header->th_off += 3;
	return tcp_header->th_off * 4;
}

size_t set_sack_permitted_with_timestamp(struct tcphdr *tcp_header)
{
	size_t header_size = tcp_header->th_off * 4;
	uint8_t *last_opt = (uint8_t *)tcp_header + header_size;
	// SACKPermitted = 2 bytes
	last_opt[0] = 0x04; // kind for SACKPermitted
	last_opt[1] = 0x02; // set the length
	last_opt += 2;	    // increment pointer
	// exact method of getting this timestamp isn't important, only that it is a 4 byte value - RFC 7323
	uint32_t now = time(NULL);
	last_opt[0] = 0x08;			  // kind for timestamp field
	last_opt[1] = 0x0a;			  // length for timestamp field
	*(uint32_t *)(last_opt + 2) = htonl(now); // set current time in correct byte order
	// final 4 bytes of timestamp field are left zeroed for the timestamp echo value
	last_opt += 10; // update our pointer 10 bytes ahead
	tcp_header->th_off += 3;
	return tcp_header->th_off * 4;
}

// sets 2x NOPs and a SACKPermitted (2 bytes) option = 4 bytes
size_t set_nop_plus_sack_permitted(struct tcphdr *tcp_header)
{
	size_t header_size = tcp_header->th_off * 4;
	uint8_t *last_opt = (uint8_t *)tcp_header + header_size;
	// NOP = 1 byte
	last_opt[0] = 0x01; // kind for NOP
	last_opt[1] = 0x01;
	last_opt += 2;
	// SACKPermitted = 2 bytes
	last_opt[0] = 0x04; // kind for SACKPermitted
	last_opt[1] = 0x02; // set the length
	last_opt += 2;	    // increment pointer
	tcp_header->th_off += 1;
	return tcp_header->th_off * 4;
}

size_t set_sack_permitted_plus_eol(struct tcphdr *tcp_header)
{
	size_t header_size = tcp_header->th_off * 4;
	uint8_t *last_opt = (uint8_t *)tcp_header + header_size;
	// SACKPermitted = 2 bytes
	last_opt[0] = 0x04; // kind for SACKPermitted
	last_opt[1] = 0x02; // set the length
	last_opt += 2;	    // increment pointer
	// EOL = 1 byte
	last_opt[0] = 0x00; // kind for EOL
	last_opt[1] = 0x00; // kind for EOL
	last_opt += 2;	    // increment pointer
	tcp_header->th_off += 1;
	return tcp_header->th_off * 4;
}

// set_tcp_options adds the relevant TCP options so ZMap-sent packets have the same TCP header as linux-sent ones
size_t set_tcp_options(struct tcphdr *tcp_header, uint8_t os_options_type)
{
	if (os_options_type == SMALLEST_PROBES_OS_OPTIONS) {
		// the minimum Ethernet payload is 46 bytes. A TCP header + IP header is 40 bytes, giving us 6 bytes to work with.
		// However, the word size is 4 bytes, so we can only use 44 or 48 bytes. Since we're trying to stay as close to the
		// minimum payload size, we'll use 4 bytes for the MSS option and the last 2 will be padded by the OS.
		set_mss_option(tcp_header);
	} else if (os_options_type == LINUX_OS_OPTIONS) {
		set_mss_option(tcp_header);
		set_sack_permitted_with_timestamp(tcp_header);
		set_nop_plus_windows_scale(tcp_header, os_options_type);
	} else if (os_options_type == BSD_OS_OPTIONS) {
		set_mss_option(tcp_header);
		set_nop_plus_windows_scale(tcp_header, os_options_type);
		set_timestamp_option_with_nops(tcp_header);
		set_sack_permitted_plus_eol(tcp_header);
	} else if (os_options_type == WINDOWS_OS_OPTIONS) {
		set_mss_option(tcp_header);
		set_nop_plus_windows_scale(tcp_header, os_options_type);
		set_nop_plus_sack_permitted(tcp_header);
	} else {
		// should not his this case
		log_fatal("packet", "unknown OS for TCP options: %d", os_options_type);
	}
	return tcp_header->th_off * 4;
}

void make_udp_header(struct udphdr *udp_header, uint16_t len)
{
	udp_header->uh_ulen = htons(len);
	// checksum ignored in IPv4 if 0
	udp_header->uh_sum = 0;
}

int icmp_helper_validate(const struct ip *ip_hdr, uint32_t len,
			 size_t min_l4_len, struct ip **probe_pkt,
			 size_t *probe_len)
{
	// We're only equipped to handle ICMP packets at this point
	assert(ip_hdr->ip_p == IPPROTO_ICMP);

	// Several ICMP responses can be generated by hosts along the way in
	// response to a non-ICMP probe packet. These include:
	//   * Source quench (ICMP_SOURCE_QUENCH)
	//   * Destination Unreachable (ICMP_DEST_UNREACH)
	//   * Redirect (ICMP_REDIRECT)
	//   * Time exceeded (ICMP_TIME_EXCEEDED)
	// In all of these cases, the IP header and first 8 bytes of the
	// original packet are included in the responses and can be used
	// to understand where the probe packet was sent.

	// Check if the response was large enough to contain an IP header
	const uint32_t min_len = 4 * ip_hdr->ip_hl + ICMP_HEADER_SIZE +
				 sizeof(struct ip) + min_l4_len;
	if (len < min_len) {
		return PACKET_INVALID;
	}
	// Check that ICMP response is one of these four
	struct icmp *icmp = (struct icmp *)((char *)ip_hdr + 4 * ip_hdr->ip_hl);
	if (!(icmp->icmp_type == ICMP_UNREACH ||
	      icmp->icmp_type == ICMP_SOURCEQUENCH ||
	      icmp->icmp_type == ICMP_REDIRECT ||
	      icmp->icmp_type == ICMP_TIMXCEED)) {
		return PACKET_INVALID;
	}
	struct ip *ip_inner = (struct ip *)((char *)icmp + ICMP_HEADER_SIZE);
	size_t inner_packet_len = len - (4 * ip_hdr->ip_hl + ICMP_HEADER_SIZE);
	// Now we know the actual inner ip length, we should recheck the buffer
	// to make sure it has enough room for the application layer packet
	if (inner_packet_len < (4 * ip_inner->ip_hl + min_l4_len)) {
		return PACKET_INVALID;
	}
	// find original destination IP and check that we sent a packet
	// to that IP address
	uint32_t dest = ip_inner->ip_dst.s_addr;
	if (!blocklist_is_allowed(dest)) {
		return PACKET_INVALID;
	}
	*probe_pkt = ip_inner;
	*probe_len = inner_packet_len;
	return PACKET_VALID;
}

void fs_add_null_icmp(fieldset_t *fs)
{
	fs_add_null(fs, "icmp_responder");
	fs_add_null(fs, "icmp_type");
	fs_add_null(fs, "icmp_code");
	fs_add_null(fs, "icmp_unreach_str");
}

void fs_add_failure_no_port(fieldset_t *fs)
{
	fs_add_null(fs, "icmp_responder");
	fs_add_null(fs, "icmp_type");
	fs_add_null(fs, "icmp_code");
	fs_add_null(fs, "icmp_unreach_str");
}

void fs_populate_icmp_from_iphdr(struct ip *ip, size_t len, fieldset_t *fs)
{
	assert(ip && "no ip header provide to fs_populate_icmp_from_iphdr");
	assert(fs && "no fieldset provided to fs_populate_icmp_from_iphdr");
	struct icmp *icmp = get_icmp_header(ip, len);
	assert(icmp);
	// ICMP unreach comes from another server (not the one we sent a
	// probe to); But we will fix up saddr to be who we sent the
	// probe to, in case you care.
	struct ip *ip_inner = get_inner_ip_header(icmp, len);
	fs_modify_string(fs, "saddr", make_ip_str(ip_inner->ip_dst.s_addr), 1);
	// Add other ICMP fields from within the header
	fs_add_string(fs, "icmp_responder", make_ip_str(ip->ip_src.s_addr), 1);
	fs_add_uint64(fs, "icmp_type", icmp->icmp_type);
	fs_add_uint64(fs, "icmp_code", icmp->icmp_code);
	if (icmp->icmp_code <= ICMP_UNREACH_PRECEDENCE_CUTOFF) {
		fs_add_constchar(fs, "icmp_unreach_str",
				 icmp_unreach_strings[icmp->icmp_code]);
	} else {
		fs_add_constchar(fs, "icmp_unreach_str", "unknown");
	}
}

// Note: caller must free return value
char *make_ip_str(uint32_t ip)
{
	struct in_addr t;
	t.s_addr = ip;
	const char *temp = inet_ntoa(t);
	char *retv = xmalloc(strlen(temp) + 1);
	strcpy(retv, temp);
	return retv;
}

// Note: caller must free return value
// added by pqm
char *make_ip_strinqname(uint32_t ip)
{
	struct in_addr t;
	t.s_addr = ip;
	const char *temp = inet_ntoa(t);
	char *retv = xmalloc(16 + 1);
	char *p;
	int count = 0;
	while(p = strsep(&temp, "."))
	{
		memset(retv + count*4, 0x03,1);
		int tmpint = atoi(p);
		sprintf(retv + count*4 + 1, "%03d", tmpint);
		count += 1;
	}
	return retv;
}

// Note: caller must free return value
char *make_ipv6_str(struct in6_addr *ipv6)
{
	char *retv = xmalloc(INET6_ADDRSTRLEN + 1);
	inet_ntop(AF_INET6, ipv6, retv, INET6_ADDRSTRLEN);
	return retv;
}

const char *icmp_unreach_strings[] = {
    "network unreachable", "host unreachable",
    "protocol unreachable", "port unreachable",
    "fragments required", "source route failed",
    "network unknown", "host unknown",
    "source host isolated", "network admin. prohibited",
    "host admin. prohibited", "network unreachable TOS",
    "host unreachable TOS", "communication admin. prohibited",
    "host presdence violation", "precedence cutoff"};
