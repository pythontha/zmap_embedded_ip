/*
 * ZMapv6 Copyright 2016 Chair of Network Architectures and Services
 * Technical University of Munich
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

// probe module for performing arbitrary UDP scans over IPv6

// Needed for asprintf
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "../../lib/includes.h"
#include "../../lib/xalloc.h"
#include "../../lib/lockfd.h"
#include "logger.h"
#include "probe_modules.h"
#include "packet.h"
#include "aesrand.h"
#include "state.h"
#include "module_udp.h"

#define MAX_UDP_PAYLOAD_LEN 1472
#define ICMP_UNREACH_HEADER_SIZE 8
#define UNUSED __attribute__((unused))

static char *udp_send_msg = NULL;
static int udp_send_msg_len = 0;
//static int udp_send_substitutions = 0;
static udp_payload_template_t *udp_template = NULL;

static const char *udp_send_msg_default = "GET / HTTP/1.1\r\nHost: www\r\n\r\n";

/*
const char *udp_unreach_strings[] = {
	"network unreachable",
	"host unreachable",
	"protocol unreachable",
	"port unreachable",
	"fragments required",
	"source route failed",
	"network unknown",
	"host unknown",
	"source host isolated",
	"network admin. prohibited",
	"host admin. prohibited",
	"network unreachable TOS",
	"host unreachable TOS",
	"communication admin. prohibited",
	"host presdence violation",
	"precedence cutoff"
};*/

const char *ipv6_udp_usage_error =
	"unknown UDP probe specification (expected file:/path or text:STRING or hex:01020304 or template:/path or template-fields)";

const unsigned char *ipv6_charset_alphanum = (unsigned char *)"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const unsigned char *ipv6_charset_alpha    = (unsigned char *)"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const unsigned char *ipv6_charset_digit    = (unsigned char *)"0123456789";
const unsigned char ipv6_charset_all[257]  = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
	0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
	0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c,
	0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
	0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
	0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
	0x00
};

static int num_ports;
#define SOURCE_PORT_VALIDATION_MODULE_DEFAULT false; // default to NOT validating source port
static bool should_validate_src_port = SOURCE_PORT_VALIDATION_MODULE_DEFAULT

probe_module_t module_ipv6_udp;

// Field definitions for template parsing and displaying usage
static uint32_t udp_num_template_field_types = 12;
static udp_payload_field_type_def_t udp_payload_template_fields[] = {
	{.name = "SADDR_N", .ftype=UDP_SADDR_N, .desc = "Source IP address in network byte order"},
	{.name = "SADDR",   .ftype=UDP_SADDR_A, .desc = "Source IP address in dotted-quad format"},
	{.name = "DADDR_N", .ftype=UDP_DADDR_N, .desc = "Destination IP address in network byte order"},
	{.name = "DADDR",   .ftype=UDP_DADDR_A, .desc = "Destination IP address in dotted-quad format"},
	{.name = "SPORT_N", .ftype=UDP_SPORT_N, .desc = "UDP source port in netowrk byte order"},
	{.name = "SPORT",   .ftype=UDP_SPORT_A, .desc = "UDP source port in ascii format"},
	{.name = "DPORT_N", .ftype=UDP_DPORT_N, .desc = "UDP destination port in network byte order"},
	{.name = "DPORT",   .ftype=UDP_DPORT_A, .desc = "UDP destination port in ascii format"},
	{.name = "RAND_BYTE",	.ftype=UDP_RAND_BYTE,	.desc = "Random bytes from 0-255"},
	{.name = "RAND_DIGIT", .ftype=UDP_RAND_DIGIT, .desc = "Random digits from 0-9"},
	{.name = "RAND_ALPHA", .ftype=UDP_RAND_ALPHA, .desc = "Random mixed-case letters (a-z)"},
	{.name = "RAND_ALPHANUM", .ftype=UDP_RAND_ALPHANUM, .desc = "Random mixed-case letters (a-z) and numbers"}
};

int ipv6_udp_global_initialize(struct state_conf *conf) {
	char *args, *c;
	int i;
	unsigned int n;

	FILE *inp;

	num_ports = conf->source_port_last - conf->source_port_first + 1;
	udp_set_num_ports(num_ports);

	udp_send_msg = strdup(udp_send_msg_default);
	udp_send_msg_len = strlen(udp_send_msg);

	// Only look at received packets destined to the specified scanning address (useful for parallel zmap scans)
	if (asprintf((char ** restrict) &module_ipv6_udp.pcap_filter, "%s && ip6 dst host %s", module_ipv6_udp.pcap_filter, conf->ipv6_source_ip) == -1) {
		return 1;
	}

	if (!(conf->probe_args && strlen(conf->probe_args) > 0))
		return(0);

	args = strdup(conf->probe_args);
	if (! args) exit(1);

	if (strcmp(args, "template-fields") == 0) {
		lock_file(stderr);
		fprintf(stderr, "%s",
			"List of allowed UDP template fields (name: description)\n\n");
		for (uint32_t i = 0; i < udp_num_template_field_types; ++i) {
			fprintf(stderr, "%s: %s\n",
				udp_payload_template_fields[i].name,
				udp_payload_template_fields[i].desc);
		}
		fprintf(stderr, "%s\n" ,"");
		unlock_file(stderr);
		exit(0);
	}

	c = strchr(args, ':');
	if (! c) {
		free(args);
		free(udp_send_msg);
		log_fatal("udp", ipv6_udp_usage_error);
		exit(1);
	}

	*c++ = 0;

	if (strcmp(args, "text") == 0) {
		free(udp_send_msg);
		udp_send_msg = strdup(c);
		udp_send_msg_len = strlen(udp_send_msg);

	} else if (strcmp(args, "file") == 0 || strcmp(args, "template") == 0) {
		inp = fopen(c, "rb");
		if (!inp) {
			free(udp_send_msg);
			log_fatal("udp", "could not open UDP data file '%s'\n", c);
			free(args);
			exit(1);
		}
		free(udp_send_msg);
		udp_send_msg = xmalloc(MAX_UDP_PAYLOAD_LEN);
		udp_send_msg_len = fread(udp_send_msg, 1, MAX_UDP_PAYLOAD_LEN, inp);
		fclose(inp);

		if (strcmp(args, "template") == 0) {
			// TODO FIXME: Templates in IPv6 are not yet supported
			log_fatal("udp", "templates not yet supported in IPv6!");
/*			udp_send_substitutions = 1;
			udp_template = udp_template_load(udp_send_msg, udp_send_msg_len);
			*/
		}

	} else if (strcmp(args, "hex") == 0) {
		udp_send_msg_len = strlen(c) / 2;
		free(udp_send_msg);
		udp_send_msg = xmalloc(udp_send_msg_len);

		for (i=0; i < udp_send_msg_len; i++) {
			if (sscanf(c + (i*2), "%2x", &n) != 1) {
				free(udp_send_msg);
				log_fatal("udp", "non-hex character: '%c'", c[i*2]);
				free(args);
				exit(1);
			}
			udp_send_msg[i] = (n & 0xff);
		}
	} else {
		log_fatal("udp", ipv6_udp_usage_error);
		free(udp_send_msg);
		free(args);
		exit(1);
	}

	if (udp_send_msg_len > MAX_UDP_PAYLOAD_LEN) {
		log_warn("udp", "warning: reducing UDP payload to %d "
				"bytes (from %d) to fit on the wire\n",
				MAX_UDP_PAYLOAD_LEN, udp_send_msg_len);
		udp_send_msg_len = MAX_UDP_PAYLOAD_LEN;
	}
	free(args);
	return EXIT_SUCCESS;
}

int ipv6_udp_global_cleanup(__attribute__((unused)) struct state_conf *zconf,
		__attribute__((unused)) struct state_send *zsend,
		__attribute__((unused)) struct state_recv *zrecv)
{
	if (udp_send_msg) {
		free(udp_send_msg);
		udp_send_msg = NULL;
	}

	if (udp_template) {
		udp_template_free(udp_template);
		udp_template = NULL;
	}

	return EXIT_SUCCESS;
}

int ipv6_udp_init_perthread(void **arg_ptr)
{
	// Seed our random number generator with the global generator
	uint32_t seed = aesrand_getword(zconf.aes);
	aesrand_t *aes = aesrand_init_from_seed(seed);
	*arg_ptr = aes;

	return EXIT_SUCCESS;
}

int ipv6_udp_prepare_packet(void *buf, macaddr_t *src, macaddr_t *gw, UNUSED void *arg_ptr)
{
	memset(buf, 0, MAX_PACKET_SIZE);
	struct ether_header *eth_header = (struct ether_header *) buf;
	make_eth_header_ethertype(eth_header, src, gw, ETHERTYPE_IPV6);
	struct ip6_hdr *ipv6_header = (struct ip6_hdr*)(&eth_header[1]);
	uint16_t payload_len = sizeof(struct udphdr) + udp_send_msg_len;
	make_ip6_header(ipv6_header, IPPROTO_UDP, payload_len);

	struct udphdr *udp_header = (struct udphdr*)(&ipv6_header[1]);
	make_udp_header(udp_header, payload_len);

	char* payload = (char*)(&udp_header[1]);

	module_ipv6_udp.max_packet_length = sizeof(struct ether_header) + sizeof(struct ip6_hdr)
				+ sizeof(struct udphdr) + udp_send_msg_len;
	assert(module_ipv6_udp.max_packet_length <= MAX_PACKET_SIZE);

	memcpy(payload, udp_send_msg, udp_send_msg_len);

	return EXIT_SUCCESS;
}

int ipv6_udp_make_packet(void *buf, size_t *buf_len, UNUSED ipaddr_n_t src_ip,
		UNUSED ipaddr_n_t dst_ip, port_n_t dport, uint8_t ttl, uint32_t *validation, int probe_num, UNUSED uint16_t ip_id, void *arg)
{
	// From module_ipv6_udp_dns
	struct ether_header *eth_header = (struct ether_header *) buf;
	struct ip6_hdr *ip6_header = (struct ip6_hdr*) (&eth_header[1]);
	struct udphdr *udp_header= (struct udphdr *) &ip6_header[1];

	ip6_header->ip6_src = ((struct in6_addr *) arg)[0];
	ip6_header->ip6_dst = ((struct in6_addr *) arg)[1];
	ip6_header->ip6_ctlun.ip6_un1.ip6_un1_hlim = ttl;
	udp_header->uh_sport = htons(get_src_port(num_ports, probe_num,
				     validation));
	udp_header->uh_dport = dport;

	// TODO FIXME
/*
	if (udp_send_substitutions) {
		char *payload = (char *) &udp_header[1];
		int payload_len = 0;

		memset(payload, 0, MAX_UDP_PAYLOAD_LEN);

		// Grab our random number generator
		aesrand_t *aes = (aesrand_t *) arg;

		// The buf is a stack var of our caller of size MAX_PACKET_SIZE
		// Recalculate the payload using the loaded template
		payload_len = ipv6_udp_template_build(udp_template, payload, MAX_UDP_PAYLOAD_LEN, ip6_header, udp_header, aes);

		// If success is zero, the template output was truncated
		if (payload_len <= 0) {
			log_fatal("udp", "UDP payload template generated an empty payload");
			exit(1);
		}

		// Update the IPv6 and UDP headers to match the new payload length
		ip6_header->ip6_ctlun.ip6_un1.ip6_un1_plen = htons(sizeof(struct udphdr) + payload_len);
		udp_header->uh_ulen = ntohs(sizeof(struct udphdr) + payload_len);
	}
*/
	udp_header->uh_sum = 0;
	udp_header->uh_sum = ipv6_payload_checksum(ntohs(udp_header->uh_ulen), &ip6_header->ip6_src, &ip6_header->ip6_dst, (unsigned short *) udp_header, IPPROTO_UDP);
	
	size_t headers_len = sizeof(struct ether_header) + sizeof(struct ip6_hdr) +
			     sizeof(struct udphdr);
	*buf_len = headers_len + udp_send_msg_len;

	return EXIT_SUCCESS;
}

void ipv6_udp_print_packet(FILE *fp, void* packet)
{
	struct ether_header *ethh = (struct ether_header *) packet;
	struct ip6_hdr *iph = (struct ip6_hdr *) &ethh[1];
	struct udphdr *udph  = (struct udphdr*) &iph[1];
	fprintf(fp, "udp { source: %u | dest: %u | checksum: %#04X }\n",
		ntohs(udph->uh_sport),
		ntohs(udph->uh_dport),
		ntohs(udph->uh_sum));
	fprintf_ipv6_header(fp, iph);
	fprintf_eth_header(fp, ethh);
	fprintf(fp, "------------------------------------------------------\n");
}

void ipv6_udp_process_packet(const u_char *packet, UNUSED uint32_t len, fieldset_t *fs,
		__attribute__((unused)) uint32_t *validation,
		__attribute__((unused)) struct timespec ts)
{
	struct ip6_hdr *ipv6_hdr = (struct ip6_hdr *) &packet[sizeof(struct ether_header)];
	if (ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_nxt == IPPROTO_UDP) {
		struct udphdr *udp  = (struct udphdr*) &ipv6_hdr[1];
		fs_add_string(fs, "classification", (char*) "udp", 0);
		fs_add_uint64(fs, "success", 1);
		fs_add_uint64(fs, "sport", ntohs(udp->uh_sport));
		fs_add_uint64(fs, "dport", ntohs(udp->uh_dport));
		fs_add_null(fs, "icmp_responder");
		fs_add_null(fs, "icmp_type");
		fs_add_null(fs, "icmp_code");
		fs_add_null(fs, "icmp_unreach_str");
		fs_add_uint64(fs, "udp_pkt_size", ntohs(udp->uh_ulen));
		// Verify that the UDP length is big enough for the header and at least one byte
		uint16_t data_len = ntohs(udp->uh_ulen);
		if (data_len > sizeof(struct udphdr)) {
			uint32_t overhead = sizeof(struct udphdr);
			uint32_t max_rlen = len - overhead;
			uint32_t max_ilen = ntohs(ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_plen) - overhead;

			// Verify that the UDP length is inside of our received buffer
			if (data_len > max_rlen) {
				data_len = max_rlen;
			}
			// Verify that the UDP length is inside of our IP packet
			if (data_len > max_ilen) {
				data_len = max_ilen;
			}
			fs_add_binary(fs, "data", data_len, (void*) &udp[1], 0);
		// Some devices reply with a zero UDP length but still return data, ignore the data
		} else {
			fs_add_null(fs, "data");
		}
	} else if (ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_nxt == IPPROTO_ICMPV6) {
		struct icmp6_hdr *icmp6 = (struct icmp6_hdr *) (&ipv6_hdr[1]);
		struct ip6_hdr *ipv6_inner = (struct ip6_hdr *) &icmp6[1];
		// ICMP unreach comes from another server (not the one we sent a probe to);
		// But we will fix up saddr to be who we sent the probe to, in case you care.
		fs_modify_string(fs, "saddr", make_ipv6_str(&ipv6_inner->ip6_dst), 1);
		fs_add_string(fs, "classification", (char*) "icmp-unreach", 0);
		fs_add_uint64(fs, "success", 0);
		fs_add_null(fs, "sport");
		fs_add_null(fs, "dport");
		fs_add_string(fs, "icmp_responder", make_ipv6_str(&ipv6_hdr->ip6_src), 1);
		fs_add_uint64(fs, "icmp_type", icmp6->icmp6_type);
		fs_add_uint64(fs, "icmp_code", icmp6->icmp6_code);
/*
		if (icmp->icmp_code <= ICMP_UNREACH_PRECEDENCE_CUTOFF) {
			fs_add_string(fs, "icmp_unreach_str",
					(char*) udp_unreach_strings[icmp->icmp_code], 0);
		} else {
			fs_add_string(fs, "icmp_unreach_str", (char *) "unknown", 0);
		}
*/
		fs_add_null(fs, "icmp_unreach_str");
		fs_add_null(fs, "udp_pkt_size");
		fs_add_null(fs, "data");
	} else {
		fs_add_string(fs, "classification", (char *) "other", 0);
		fs_add_uint64(fs, "success", 0);
		fs_add_null(fs, "sport");
		fs_add_null(fs, "dport");
		fs_add_null(fs, "icmp_responder");
		fs_add_null(fs, "icmp_type");
		fs_add_null(fs, "icmp_code");
		fs_add_null(fs, "icmp_unreach_str");
		fs_add_null(fs, "udp_pkt_size");
		fs_add_null(fs, "data");
	}
}


int _ipv6_udp_validate_packet(const struct ip *ip_hdr, uint32_t len,
		UNUSED uint32_t *src_ip, uint32_t *validation, const struct port_conf *ports)
{
	struct ip6_hdr *ipv6_hdr = (struct ip6_hdr *) ip_hdr;
/*
	if (ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_nxt != IPPROTO_UDP) {
		return 0;
	}
*/
	if ((ntohs(ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_plen)) > len) {
		// buffer not large enough to contain expected UDP header, i.e. IPv6 payload
		return 0;
	}
	if (!ipv6_udp_validate_packet(ipv6_hdr, len, NULL, validation, num_ports, should_validate_src_port, ports)) {
		return 0;
	}
	return 1;
}

// Add a new field to the template
void ipv6_udp_template_add_field(udp_payload_template_t *t,
	udp_payload_field_type_t ftype, unsigned int length, char *data)
{
	udp_payload_field_t *c;

	t->fcount++;
	t->fields = xrealloc(t->fields, sizeof(udp_payload_field_t) * t->fcount);
	if (! t->fields) {
		exit(1);
	}

	t->fields[t->fcount - 1] = xmalloc(sizeof(udp_payload_field_t));
	c = t->fields[t->fcount - 1];

	if (! c) {
		exit(1);
	}

	c->ftype	= ftype;
	c->length = length;
	c->data	 = data;
}

// Free all buffers held by the payload template, including its own
void ipv6_udp_template_free(udp_payload_template_t *t)
{
	unsigned int x;
	for (x=0; x < t->fcount; x++) {
		if (t->fields[x]->data) {
			free(t->fields[x]->data);
			t->fields[x]->data = NULL;
		}
		free(t->fields[x]);
		t->fields[x] = NULL;
	}
	free(t->fields);
	t->fields = NULL;
	t->fcount = 0;
	free(t);
}

int ipv6_udp_random_bytes(char *dst, int len, const unsigned char *charset,
		int charset_len, aesrand_t *aes) {
	int i;
	for(i=0; i<len; i++)
		*dst++ = charset[ (aesrand_getword(aes) & 0xFFFFFFFF) % charset_len ];
	return i;
}
/*
int ipv6_udp_template_build(udp_payload_template_t *t, char *out, unsigned int len,
	struct ip6_hdr *ip6_header, struct udphdr *udp_hdr, aesrand_t *aes)
{
	udp_payload_field_t *c;
	char *p;
	char *max;
	char tmp[256];
	int full = 0;
	unsigned int x, y;
	uint32_t *u32;
	uint16_t *u16;

	max = out + len;
	p	 = out;

	for (x=0; x < t->fcount; x++) {
		c = t->fields[x];

		// Exit the processing loop if our packet buffer would overflow
		if (p+c->length >= max) {
			full = 1;
			return 0;
		}

		switch (c->ftype) {

		// These fields have a specified output length value

			case UDP_DATA:
				if (! (c->data && c->length))
					break;
				memcpy(p, c->data, c->length);
				p += c->length;
				break;

			case UDP_RAND_DIGIT:
				p += ipv6_udp_random_bytes(p, c->length, ipv6_charset_digit, 10, aes);
				break;

			case UDP_RAND_ALPHA:
				p += ipv6_udp_random_bytes(p, c->length, ipv6_charset_alpha, 52, aes);
				break;

			case UDP_RAND_ALPHANUM:
				p += ipv6_udp_random_bytes(p, c->length, ipv6_charset_alphanum, 62, aes);
				break;

			case UDP_RAND_BYTE:
				p += ipv6_udp_random_bytes(p, c->length, ipv6_charset_all, 256, aes);
				break;

			// These fields need to calculate size on their own

			// TODO: Condense these case statements to remove redundant code
			case UDP_SADDR_A:
				if ( p + 15 >= max) {
					full = 1;
					break;
				}
				// Write to stack and then memcpy in order to properly track length
				inet_ntop(AF_INET, (char *)&ip_hdr->ip_src, tmp, sizeof(tmp)-1);
				memcpy(p, tmp, strlen(tmp));
				p += strlen(tmp);
				break;

			case UDP_DADDR_A:
				if ( p + 15 >= max) {
					full = 1;
					break;
				}
				// Write to stack and then memcpy in order to properly track length
				inet_ntop(AF_INET, (char *)&ip_hdr->ip_dst, tmp, sizeof(tmp)-1);
				memcpy(p, tmp, strlen(tmp));
				p += strlen(tmp);
				break;

			case UDP_SADDR_N:
				if ( p + 4 >= max) {
					full = 1;
					break;
				}

				u32 = (uint32_t *)p;
				*u32 = ip_hdr->ip_src.s_addr;
				p += 4;
				break;

			case UDP_DADDR_N:
				if ( p + 4 >= max) {
					full = 1;
					break;
				}
				u32 = (uint32_t *)p;
				*u32 = ip_hdr->ip_dst.s_addr;
				p += 4;
				break;

			case UDP_SPORT_N:
				if ( p + 2 >= max) {
					full = 1;
					break;
				}
				u16 = (uint16_t *)p;
				*u16 = udp_hdr->uh_sport;
				p += 2;
				break;

			case UDP_DPORT_N:
				if ( p + 2 >= max) {
					full = 1;
					break;
				}
				u16 = (uint16_t *)p;
				*u16 = udp_hdr->uh_sport;
				p += 2;
				break;

			case UDP_SPORT_A:
				if ( p + 5 >= max) {
					full = 1;
					break;
				}
				y = snprintf(tmp, 6, "%d", ntohs(udp_hdr->uh_sport));
				memcpy(p, tmp, y);
				p += y;
				break;

			case UDP_DPORT_A:
				if ( p + 5 >= max) {
					full = 1;
					break;
				}
				y = snprintf(tmp, 6, "%d", ntohs(udp_hdr->uh_sport));
				memcpy(p, tmp, y);
				p += y;
				break;
		}

		// Bail out if our packet buffer would overflow
		if (full == 1) {
			return 0;
		}
	}

	return p - out - 1;
}

// Convert a string field name to a field type, parsing any specified length value
int ipv6_udp_template_field_lookup(char *vname, udp_payload_field_t *c)
{
	char *param;
	unsigned int f;
	unsigned int olen = 0;
	unsigned int fcount = sizeof(udp_payload_template_fields)/sizeof(udp_payload_template_fields[0]);

	param = strstr((const char*)vname, "=");
	if (param) {
		*param = '\0';
		param++;
	}

	// Most field types treat their parameter as a generator output length
	// unless it is ignored (ADDR, PORT, etc).
	if (param) {
		olen = atoi((const char *)param);
	}

	// Find a field that matches the
	for (f=0; f<fcount; f++) {

		if (strcmp((char *)vname, udp_payload_template_fields[f].name) == 0) {
			c->ftype	= udp_payload_template_fields[f].ftype;
			c->length = olen;
			c->data	 = NULL;
			return 1;
		}
	}

	// No match, skip and treat it as a data field
	return 0;
}

// Allocate a payload template and populate it by parsing a template file as a binary buffer
udp_payload_template_t * ipv6_udp_template_load(char *buf, unsigned int len)
{
	udp_payload_template_t *t = xmalloc(sizeof(udp_payload_template_t));

	// The last $ we encountered outside of a field specifier
	char *dollar = NULL;

	// The last { we encountered outside of a field specifier
	char *lbrack = NULL;

	// Track the start pointer of a data field (static)
	char *s = buf;

	// Track the index into the template
	char *p = buf;

	char *tmp;
	unsigned int tlen;

	udp_payload_field_t c;

	t->fcount = 0;
	t->fields = NULL;

	while (p < (buf+len))
	{
		switch(*p){

			case '$':
				if ( (dollar && !lbrack) || !dollar) {
					dollar = p;
				}
				p++;
				continue;

			case '{':
				if (dollar && !lbrack) {
					lbrack = p;
				}

				p++;
				continue;

			case '}':
				if (! (dollar && lbrack)) {
					p++;
					continue;
				}

				// Store the leading bytes before ${ as a data field
				tlen = dollar - s;
				if ( tlen > 0) {
					tmp = xmalloc(tlen);
					memcpy(tmp, s, tlen);
					udp_template_add_field(t, UDP_DATA, tlen, tmp);
				}

				tmp = xcalloc(1, p-lbrack);
				memcpy(tmp, lbrack+1, p-lbrack-1);

				if (udp_template_field_lookup(tmp, &c)) {
					udp_template_add_field(t, c.ftype, c.length, c.data);

					// Push the pointer past the } if this was a valid variable
					s = p + 1;
				} else {

					// Rewind back to the ${ sequence if this was an invalid variable
					s = dollar;
				}

				free(tmp);
				break;

			default:
				if (dollar && lbrack) {
					p++;
					continue;
				}
		}

		dollar = NULL;
		lbrack = NULL;

		p++;
	}

	// Store the trailing bytes as a final data field
	if ( s < p ) {
		tlen = p - s;
		tmp = xmalloc(tlen);
		memcpy(tmp, s, tlen);
		udp_template_add_field(t, UDP_DATA, tlen, tmp);
	}

	return t;
}
*/
static fielddef_t fields[] = {
	{.name = "classification", .type="string", .desc = "packet classification"},
	{.name = "success", .type="int", .desc = "is response considered success"},
	{.name = "sport", .type = "int", .desc = "UDP source port"},
	{.name = "dport", .type = "int", .desc = "UDP destination port"},
	{.name = "icmp_responder", .type = "string", .desc = "Source IP of ICMP_UNREACH message"},
	{.name = "icmp_type", .type = "int", .desc = "icmp message type"},
	{.name = "icmp_code", .type = "int", .desc = "icmp message sub type code"},
	{.name = "icmp_unreach_str", .type = "string", .desc = "for icmp_unreach responses, the string version of icmp_code (e.g. network-unreach)"},
	{.name = "udp_pkt_size", .type="int", .desc = "UDP packet length"},
	{.name = "data", .type="binary", .desc = "UDP payload"}
};

probe_module_t module_ipv6_udp = {
	.name = "ipv6_udp",
	.max_packet_length = 1,
	.pcap_filter = "ip6 proto 17 || icmp6",
	.pcap_snaplen = 1500,
	.port_args = 1,
	.thread_initialize = &ipv6_udp_init_perthread,
	.global_initialize = &ipv6_udp_global_initialize,
	.prepare_packet = &ipv6_udp_prepare_packet,
	.make_packet = &ipv6_udp_make_packet,
	.print_packet = &ipv6_udp_print_packet,
	.validate_packet = &_ipv6_udp_validate_packet,
	.process_packet = &ipv6_udp_process_packet,
	.close = &ipv6_udp_global_cleanup,
	.helptext = "Probe module that sends IPv6+UDP packets to hosts. Packets can "
	            "optionally be templated based on destination host. Specify"
	            " packet file with --probe-args=file:/path_to_packet_file "
	            "and templates with template:/path_to_template_file.",
	.fields = fields,
	.numfields = sizeof(fields)/sizeof(fields[0])
};
