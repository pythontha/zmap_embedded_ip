/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>

#include "../../lib/includes.h"
#include "../../lib/logger.h"
#include "../../lib/xalloc.h"
#include "../fieldset.h"
#include "probe_modules.h"
#include "packet.h"

extern probe_module_t module_tcp_synscan;
extern probe_module_t module_tcp_synackscan;
extern probe_module_t module_icmp_echo;
extern probe_module_t module_icmp_echo_time;
extern probe_module_t module_udp;
extern probe_module_t module_ntp;
extern probe_module_t module_upnp;
extern probe_module_t module_dns;
extern probe_module_t module_ipip;
extern probe_module_t module_bacnet;
extern probe_module_t module_tcp_synopt;
extern probe_module_t module_ipv6_tcp_synscan;
extern probe_module_t module_ipv6_tcp_synopt;
extern probe_module_t module_ipv6_udp;
extern probe_module_t module_ipv6_udp_dns;
extern probe_module_t module_icmp6_echoscan;
extern probe_module_t module_quic_initial;
extern probe_module_t module_ipv6_quic_initial;

// ADD YOUR MODULE HERE
extern probe_module_t module_icmp6_echo_time_novalidation;

probe_module_t *probe_modules[] = {
	&module_tcp_synscan,
	&module_tcp_synackscan,
	&module_icmp_echo,
	&module_icmp_echo_time,
	&module_udp,
	&module_ntp,
	&module_upnp,
	&module_dns,
	&module_bacnet,
	&module_ipip,

	&module_tcp_synopt,
	&module_ipv6_tcp_synscan,
	&module_ipv6_tcp_synopt,
	&module_ipv6_udp,
	&module_ipv6_udp_dns,
	&module_icmp6_echoscan,
	&module_quic_initial,
	&module_ipv6_quic_initial,
	// ADD YOUR MODULE HERE
	&module_icmp6_echo_time_novalidation
};

probe_module_t *get_probe_module_by_name(const char *name)
{
	int len = (int)(sizeof(probe_modules) / sizeof(probe_modules[0]));
	for (int i = 0; i < len; i++) {
		if (!strcmp(probe_modules[i]->name, name)) {
			return probe_modules[i];
		}
	}
	return NULL;
}

void print_probe_modules(void)
{
	int len = (int)(sizeof(probe_modules) / sizeof(probe_modules[0]));
	for (int i = 0; i < len; i++) {
		printf("%s\n", probe_modules[i]->name);
	}
}

void fs_add_ip_fields(fieldset_t *fs, struct ip *ip)
{
	// WARNING: you must update fs_ip_fields_len  as well
	// as the definitions set (ip_fiels) if you
	// change the fields added below:
	fs_add_string(fs, "saddr", make_ip_str(ip->ip_src.s_addr), 1);
	fs_add_uint64(fs, "saddr_raw", (uint64_t)ip->ip_src.s_addr);
	fs_add_string(fs, "daddr", make_ip_str(ip->ip_dst.s_addr), 1);
	fs_add_uint64(fs, "daddr_raw", (uint64_t)ip->ip_dst.s_addr);
	fs_add_uint64(fs, "ipid", ntohs(ip->ip_id));
	fs_add_uint64(fs, "ttl", ip->ip_ttl);
}

void fs_add_ipv6_fields(fieldset_t *fs, struct ip6_hdr *ipv6_hdr)
{
	// WARNING: you must update fs_ip_fields_len  as well
	// as the definitions set (ip_fiels) if you
	// change the fields added below:
	fs_add_string(fs, "saddr", make_ipv6_str(&(ipv6_hdr->ip6_src)), 1);
// TODO FIXME
//	fs_add_uint64(fs, "saddr-raw", (uint64_t) ip->ip_src.s_addr);
	fs_add_uint64(fs, "saddr_raw", (uint64_t) 0);
	fs_add_string(fs, "daddr", make_ipv6_str(&(ipv6_hdr->ip6_dst)), 1);
//	fs_add_uint64(fs, "daddr_raw", (uint64_t) ip->ip_dst.s_addr);
	fs_add_uint64(fs, "daddr_raw", (uint64_t) 0);
//	fs_add_uint64(fs, "ipid", ntohs(ipv6->ip_id));
	fs_add_uint64(fs, "ipid", 0);
	fs_add_uint64(fs, "ttl", ipv6_hdr->ip6_ctlun.ip6_un1.ip6_un1_hlim);
}

#define TIMESTR_LEN 55

void fs_add_system_fields(fieldset_t *fs, int is_repeat, int in_cooldown, const struct timespec ts)
{
	fs_add_bool(fs, "repeat", is_repeat);
	fs_add_bool(fs, "cooldown", in_cooldown);

	char *timestr = xmalloc(TIMESTR_LEN + 1);
	char *timestr_ms = xmalloc(TIMESTR_LEN + 1);
	struct tm *ptm = localtime(&ts.tv_sec);
	strftime(timestr, TIMESTR_LEN, "%Y-%m-%dT%H:%M:%S.%%03d%z", ptm);
	snprintf(timestr_ms, TIMESTR_LEN, timestr, ts.tv_nsec / 1000000);
	free(timestr);
	fs_add_string(fs, "timestamp_str", timestr_ms, 1);
	fs_add_uint64(fs, "timestamp_ts", (uint64_t)ts.tv_sec);
	fs_add_uint64(fs, "timestamp_us", (uint64_t)(ts.tv_nsec/1000));
}

int ip_fields_len = 6;
fielddef_t ip_fields[] = {
    {.name = "saddr",
     .type = "string",
     .desc = "source IP address of response"},
    {.name = "saddr_raw",
     .type = "int",
     .desc = "network order integer form of source IP address"},
    {.name = "daddr",
     .type = "string",
     .desc = "destination IP address of response"},
    {.name = "daddr_raw",
     .type = "int",
     .desc = "network order integer form of destination IP address"},
    {.name = "ipid",
     .type = "int",
     .desc = "IP identification number of response"},
    {.name = "ttl", .type = "int", .desc = "time-to-live of response packet"}};

int sys_fields_len = 5;
fielddef_t sys_fields[] = {
    {.name = "repeat",
     .type = "bool",
     .desc = "Is response a repeat response from host"},
    {.name = "cooldown",
     .type = "bool",
     .desc = "Was response received during the cooldown period"},
    {.name = "timestamp_str",
     .type = "string",
     .desc = "timestamp of when response arrived in ISO8601 format."},
    {.name = "timestamp_ts",
     .type = "int",
     .desc = "timestamp of when response arrived in seconds since Epoch"},
    {.name = "timestamp_us",
     .type = "int",
     .desc =
	 "microsecond part of timestamp (e.g. microseconds since 'timestamp-ts')"}};
