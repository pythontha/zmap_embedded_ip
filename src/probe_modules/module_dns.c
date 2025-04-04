/*
 * ZMap Copyright 2015 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

// Module for scanning for open UDP DNS resolvers.
//
// This module optionally takes in an argument of the form "TYPE,QUESTION"
// (e.g. "A,google.com").
//
// Given no arguments it will default to asking for an A record for
// www.google.com.
//
// This module does minimal answer verification. It only verifies that the
// response roughly looks like a DNS response. It will not, for example,
// require the QR bit be set to 1. All such analysis should happen offline.
// Specifically, to be included in the output it requires:
//   - That the response packet is >= the query packet.
//   - That the ports match and the packet is complete.
// To be marked as success it also requires:
//   - That the response bytes that should be the ID field matches the send bytes.
//   - That the response bytes that should be question match send bytes.
// To be marked as app_success it also requires:
//   - That the QR bit be 1 and rcode == 0.
//
// Usage:
//     zmap -p 53 --probe-module=dns --probe-args="ANY,www.example.com"
//         -O json --output-fields=* 8.8.8.8
//
// We also support multiple questions, of the form:
// "A,example.com;AAAA,www.example.com" This requires --probes=X, where X
// is a multiple of the number of questions in --probe-args, and either
// --output-filter="" or --output-module=csv to remove the implicit
// "filter_duplicates" configuration flag.
//

#include "module_dns.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "../../lib/includes.h"
#include "../../lib/random.h"
#include "../../lib/xalloc.h"
#include "probe_modules.h"
#include "packet.h"
#include "logger.h"
#include "module_udp.h"
#include "../fieldset.h"

#define DNS_PAYLOAD_LEN_LIMIT 512 // This is arbitrary
#define PCAP_SNAPLEN 1500	  // This is even more arbitrary
#define MAX_QTYPE 255
#define ICMP_UNREACH_HEADER_SIZE 8
#define BAD_QTYPE_STR "BAD QTYPE"
#define BAD_QTYPE_VAL -1
#define MAX_LABEL_RECURSION 10
#define DNS_QR_ANSWER 1
#define SOURCE_PORT_VALIDATION_MODULE_DEFAULT true; // default to validating source port
static bool should_validate_src_port = SOURCE_PORT_VALIDATION_MODULE_DEFAULT

// Note: each label has a max length of 63 bytes. So someone has to be doing
// something really annoying. Will raise a warning.
// THIS INCLUDES THE NULL BYTE
#define MAX_NAME_LENGTH 512

// zmap boilerplate
probe_module_t module_dns;
static int num_ports;

const char default_domain[] = "www.google.com";
const uint16_t default_qtype = DNS_QTYPE_A;
const uint8_t default_rdbit = 0xFF;

static char **dns_packets;
static uint16_t *dns_packet_lens; // Not including udp header
static uint16_t *qname_lens;
static char **qnames;
static uint16_t *qtypes;
static int num_questions = 0; // How many DNS questions to query. Note: There's a requirement that probes is a multiple of DNS questions
// necessary to null-terminate these since strtrk_r can take multiple delimitors as a char*, and since these are contiguous in memory,
// they were being used jointly when the intention is to use only one at a time.
static const char *probe_arg_delimitor = ";\0";
static const char *domain_qtype_delimitor = ",\0";
static const char *rn_delimitor = ":\0";

static uint8_t *rdbits;
const char *qopts_rn = "nr"; // used in query to disable recursion bit in DNS header

/* Array of qtypes we support. Jumping through some hoops (1 level of
 * indirection) so the per-packet processing time is fast. Keep this in sync
 * with: dns_qtype (.h) qtype_strid_to_qtype (below) qtype_qtype_to_strid
 * (below, and setup_qtype_str_map())
 */
const char *qtype_strs[] = {"A", "NS", "CNAME", "SOA", "PTR",
			    "MX", "TXT", "AAAA", "RRSIG", "ALL"};
const int qtype_strs_len = 10;

const dns_qtype qtype_strid_to_qtype[] = {
    DNS_QTYPE_A, DNS_QTYPE_NS, DNS_QTYPE_CNAME, DNS_QTYPE_SOA,
    DNS_QTYPE_PTR, DNS_QTYPE_MX, DNS_QTYPE_TXT, DNS_QTYPE_AAAA,
    DNS_QTYPE_RRSIG, DNS_QTYPE_ALL};

int8_t qtype_qtype_to_strid[256] = {BAD_QTYPE_VAL};

void setup_qtype_str_map(void)
{
	qtype_qtype_to_strid[DNS_QTYPE_A] = 0;
	qtype_qtype_to_strid[DNS_QTYPE_NS] = 1;
	qtype_qtype_to_strid[DNS_QTYPE_CNAME] = 2;
	qtype_qtype_to_strid[DNS_QTYPE_SOA] = 3;
	qtype_qtype_to_strid[DNS_QTYPE_PTR] = 4;
	qtype_qtype_to_strid[DNS_QTYPE_MX] = 5;
	qtype_qtype_to_strid[DNS_QTYPE_TXT] = 6;
	qtype_qtype_to_strid[DNS_QTYPE_AAAA] = 7;
	qtype_qtype_to_strid[DNS_QTYPE_RRSIG] = 8;
	qtype_qtype_to_strid[DNS_QTYPE_ALL] = 9;
}

static uint16_t qtype_str_to_code(const char *str)
{
	for (int i = 0; i < qtype_strs_len; i++) {
		if (strcmp(qtype_strs[i], str) == 0)
			return qtype_strid_to_qtype[i];
	}
	return 0;
}

static uint16_t domain_to_qname(char **qname_handle, const char *domain)
{
	// String + 1byte header + null byte
	uint16_t len = strlen(domain) + 1 + 1;
	char *qname = xmalloc(len);
	// Add a . before the domain. This will make the following simpler.
	qname[0] = '.';
	// Move the domain into the qname buffer.
	strcpy(qname + 1, domain);
	for (int i = 0; i < len; i++) {
		if (qname[i] == '.') {
			int j;
			for (j = i + 1; j < (len - 1); j++) {
				if (qname[j] == '.') {
					break;
				}
			}
			qname[i] = j - i - 1;
		}
	}
	*qname_handle = qname;
	assert((*qname_handle)[len - 1] == '\0');
	return len;
}

static int build_global_dns_packets(char *domains[], int num_domains, size_t *max_len)
{
	size_t _max_len = 0;
	for (int i = 0; i < num_domains; i++) {

		qname_lens[i] = domain_to_qname(&qnames[i], domains[i]);
		log_debug("dns", "added by pqm");
		if (domains[i] != (char *)default_domain) {
			free(domains[i]);
		}
		uint16_t len = sizeof(dns_header) + qname_lens[i] +
			       sizeof(dns_question_tail);
		dns_packet_lens[i] = len;
		if (len > _max_len) {
			_max_len = len;
		}

		if (dns_packet_lens[i] > DNS_PAYLOAD_LEN_LIMIT) {
			log_fatal("dns",
				  "DNS packet bigger (%d) than our limit (%d)",
				  dns_packet_lens[i], DNS_PAYLOAD_LEN_LIMIT);
			return EXIT_FAILURE;
		}

		dns_packets[i] = xmalloc(dns_packet_lens[i]);

		dns_header *dns_header_p = (dns_header *)dns_packets[i];
		char *qname_p = dns_packets[i] + sizeof(dns_header);
		dns_question_tail *tail_p =
		    (dns_question_tail *)(dns_packets[i] + sizeof(dns_header) +
					  qname_lens[i]);

		// All other header fields should be 0. Except id, which we set
		// per thread. Please recurse as needed.
		dns_header_p->rd = rdbits[i];

		// We have 1 question
		dns_header_p->qdcount = htons(1);
		memcpy(qname_p, qnames[i], qname_lens[i]);
		// Set the qtype to what we passed from args
		tail_p->qtype = htons(qtypes[i]);
		// Set the qclass to The Internet
		tail_p->qclass = htons(0x01);
	}
	*max_len = _max_len;
	return EXIT_SUCCESS;
}

static uint16_t get_name_helper(const char *data, uint16_t data_len,
				const char *payload, uint16_t payload_len,
				char *name, uint16_t name_len,
				uint16_t recursion_level)
{
	log_trace("dns",
		  "_get_name_helper IN, datalen: %d namelen: %d recursion: %d",
		  data_len, name_len, recursion_level);
	if (data_len == 0 || name_len == 0 || payload_len == 0) {
		log_trace(
		    "dns",
		    "_get_name_helper OUT, err. 0 length field. datalen %d namelen %d payloadlen %d",
		    data_len, name_len, payload_len);
		return 0;
	}
	if (recursion_level > MAX_LABEL_RECURSION) {
		log_trace("dns", "_get_name_helper OUT. ERR, MAX RECURSION");
		return 0;
	}
	uint16_t bytes_consumed = 0;
	// The start of data is either a sequence of labels or a ptr.
	while (data_len > 0) {
		uint8_t byte = data[0];
		// Is this a pointer?
		if (byte >= 0xc0) {
			log_trace("dns", "_get_name_helper, ptr encountered");
			// Do we have enough bytes to check ahead?
			if (data_len < 2) {
				log_trace(
				    "dns",
				    "_get_name_helper OUT. ptr byte encountered. No offset. ERR.");
				return 0;
			}
			// No. ntohs isn't needed here. It's because of
			// the upper 2 bits indicating a pointer.
			uint16_t offset =
			    ((byte & 0x03) << 8) | (uint8_t)data[1];
			log_trace("dns", "_get_name_helper. ptr offset 0x%x",
				  offset);
			if (offset >= payload_len) {
				log_trace(
				    "dns",
				    "_get_name_helper OUT. offset exceeded payload len %d ERR",
				    payload_len);
				return 0;
			}

			// We need to add a dot if we are:
			// -- Not first level recursion.
			// -- have consumed bytes
			if (recursion_level > 0 || bytes_consumed > 0) {

				if (name_len < 1) {
					log_warn(
					    "dns",
					    "Exceeded static name field allocation.");
					return 0;
				}

				name[0] = '.';
				name++;
				name_len--;
			}
			uint16_t rec_bytes_consumed = get_name_helper(
			    payload + offset, payload_len - offset, payload,
			    payload_len, name, name_len, recursion_level + 1);
			// We are done so don't bother to increment the
			// pointers.
			if (rec_bytes_consumed == 0) {
				log_trace(
				    "dns",
				    "_get_name_helper OUT. rec level %d failed",
				    recursion_level);
				return 0;
			} else {
				bytes_consumed += 2;
				log_trace(
				    "dns",
				    "_get_name_helper OUT. rec level %d success. "
				    "%d rec bytes consumed. %d bytes consumed.",
				    recursion_level, rec_bytes_consumed,
				    bytes_consumed);
				return bytes_consumed;
			}
		} else if (byte == '\0') {
			// don't bother with pointer incrementation. We're done.
			bytes_consumed += 1;
			log_trace(
			    "dns",
			    "_get_name_helper OUT. rec level %d success. %d bytes consumed.",
			    recursion_level, bytes_consumed);
			return bytes_consumed;
		} else {
			log_trace("dns",
				  "_get_name_helper, segment 0x%hx encountered",
				  byte);
			// We've now consumed a byte.
			++data;
			--data_len;
			// Mark byte consumed after we check for first
			// iteration. Do we have enough data left (must have
			// null byte too)?
			if ((byte + 1) > data_len) {
				log_trace(
				    "dns",
				    "_get_name_helper OUT. ERR. Not enough data for segment %hd");
				return 0;
			}
			// If we've consumed any bytes and are in a label, we're
			// in a label chain. We need to add a dot.
			if (bytes_consumed > 0) {

				if (name_len < 1) {
					log_warn(
					    "dns",
					    "Exceeded static name field allocation.");
					return 0;
				}

				name[0] = '.';
				name++;
				name_len--;
			}
			// Now we've consumed a byte.
			++bytes_consumed;
			// Did we run out of our arbitrary buffer?
			if (byte > name_len) {
				log_warn(
				    "dns",
				    "Exceeded static name field allocation.");
				return 0;
			}

			assert(data_len > 0);
			memcpy(name, data, byte);
			name += byte;
			name_len -= byte;
			data_len -= byte;
			data += byte;
			bytes_consumed += byte;
			// Handled in the byte+1 check above.
			assert(data_len > 0);
		}
	}
	// We should never get here.
	// For each byte we either have:
	// -- a ptr, which terminates
	// -- a null byte, which terminates
	// -- a segment length which either terminates or ensures we keep
	// looping
	assert(0);
	return 0;
}

// data: Where we are in the dns payload
// payload: the entire udp payload
static char *get_name(const char *data, uint16_t data_len, const char *payload,
		      uint16_t payload_len, uint16_t *bytes_consumed)
{
	log_trace("dns", "call to get_name, data_len: %d", data_len);
	char *name = xmalloc(MAX_NAME_LENGTH);
	*bytes_consumed = get_name_helper(data, data_len, payload, payload_len,
					  name, MAX_NAME_LENGTH - 1, 0);
	if (*bytes_consumed == 0) {
		free(name);
		return NULL;
	}
	// Our memset ensured null byte.
	assert(name[MAX_NAME_LENGTH - 1] == '\0');
	log_trace(
	    "dns",
	    "return success from get_name, bytes_consumed: %d, string: %s",
	    *bytes_consumed, name);
	return name;
}

static bool process_response_question(char **data, uint16_t *data_len,
				      const char *payload, uint16_t payload_len,
				      fieldset_t *list)
{
	// Payload is the start of the DNS packet, including header
	// data is handle to the start of this RR
	// data_len is a pointer to the how much total data we have to work
	// with. This is awful. I'm bad and should feel bad.
	uint16_t bytes_consumed = 0;
	char *question_name =
	    get_name(*data, *data_len, payload, payload_len, &bytes_consumed);
	// Error.
	if (question_name == NULL) {
		return true;
	}
	assert(bytes_consumed > 0);
	if ((bytes_consumed + sizeof(dns_question_tail)) > *data_len) {
		free(question_name);
		return true;
	}
	dns_question_tail *tail = (dns_question_tail *)(*data + bytes_consumed);
	uint16_t qtype = ntohs(tail->qtype);
	uint16_t qclass = ntohs(tail->qclass);
	// Build our new question fieldset
	fieldset_t *qfs = fs_new_fieldset(NULL);
	fs_add_unsafe_string(qfs, "name", question_name, 1);
	fs_add_uint64(qfs, "qtype", qtype);
	if (qtype > MAX_QTYPE || qtype_qtype_to_strid[qtype] == BAD_QTYPE_VAL) {
		fs_add_string(qfs, "qtype_str", (char *)BAD_QTYPE_STR, 0);
	} else {
		// I've written worse things than this 3rd arg. But I want to be
		// fast.
		fs_add_string(qfs, "qtype_str",
			      (char *)qtype_strs[qtype_qtype_to_strid[qtype]],
			      0);
	}
	fs_add_uint64(qfs, "qclass", qclass);
	// Now we're adding the new fs to the list.
	fs_add_fieldset(list, NULL, qfs);
	// Now update the pointers.
	*data = *data + bytes_consumed + sizeof(dns_question_tail);
	*data_len = *data_len - bytes_consumed - sizeof(dns_question_tail);
	return false;
}

static bool process_response_answer(char **data, uint16_t *data_len,
				    const char *payload, uint16_t payload_len,
				    fieldset_t *list)
{
	log_trace("dns", "call to process_response_answer, data_len: %d",
		  *data_len);
	// Payload is the start of the DNS packet, including header
	// data is handle to the start of this RR
	// data_len is a pointer to the how much total data we have to work
	// with. This is awful. I'm bad and should feel bad.
	uint16_t bytes_consumed = 0;
	char *answer_name =
	    get_name(*data, *data_len, payload, payload_len, &bytes_consumed);
	// Error.
	if (answer_name == NULL) {
		return true;
	}
	assert(bytes_consumed > 0);
	if ((bytes_consumed + sizeof(dns_answer_tail)) > *data_len) {
		free(answer_name);
		return true;
	}
	dns_answer_tail *tail = (dns_answer_tail *)(*data + bytes_consumed);
	uint16_t type = ntohs(tail->type);
	uint16_t class = ntohs(tail->class);
	uint32_t ttl = ntohl(tail->ttl);
	uint16_t rdlength = ntohs(tail->rdlength);
	char *rdata = tail->rdata;

	if ((rdlength + bytes_consumed + sizeof(dns_answer_tail)) > *data_len) {
		free(answer_name);
		return true;
	}
	// Build our new question fieldset
	fieldset_t *afs = fs_new_fieldset(NULL);
	fs_add_unsafe_string(afs, "name", answer_name, 1);
	fs_add_uint64(afs, "type", type);
	if (type > MAX_QTYPE || qtype_qtype_to_strid[type] == BAD_QTYPE_VAL) {
		fs_add_string(afs, "type_str", (char *)BAD_QTYPE_STR, 0);
	} else {
		// I've written worse things than this 3rd arg. But I want to be
		// fast.
		fs_add_string(afs, "type_str",
			      (char *)qtype_strs[qtype_qtype_to_strid[type]],
			      0);
	}
	fs_add_uint64(afs, "class", class);
	fs_add_uint64(afs, "ttl", ttl);
	fs_add_uint64(afs, "rdlength", rdlength);

	// XXX Fill this out for the other types we care about.
	if (type == DNS_QTYPE_NS || type == DNS_QTYPE_CNAME) {
		uint16_t rdata_bytes_consumed = 0;
		char *rdata_name = get_name(rdata, rdlength, payload,
					    payload_len, &rdata_bytes_consumed);
		if (rdata_name == NULL) {
			fs_add_uint64(afs, "rdata_is_parsed", 0);
			fs_add_binary(afs, "rdata", rdlength, rdata, 0);
		} else {
			fs_add_uint64(afs, "rdata_is_parsed", 1);
			fs_add_unsafe_string(afs, "rdata", rdata_name, 1);
		}
	} else if (type == DNS_QTYPE_MX) {
		uint16_t rdata_bytes_consumed = 0;
		if (rdlength <= 4) {
			fs_add_uint64(afs, "rdata_is_parsed", 0);
			fs_add_binary(afs, "rdata", rdlength, rdata, 0);
		} else {
			char *rdata_name =
			    get_name(rdata + 2, rdlength - 2, payload,
				     payload_len, &rdata_bytes_consumed);
			if (rdata_name == NULL) {
				fs_add_uint64(afs, "rdata_is_parsed", 0);
				fs_add_binary(afs, "rdata", rdlength, rdata, 0);
			} else {
				// (largest value 16bit) + " " + answer + null
				char *rdata_with_pref =
				    xmalloc(5 + 1 + strlen(rdata_name) + 1);

				uint8_t num_printed =
				    snprintf(rdata_with_pref, 6, "%hu ",
					     ntohs(*(uint16_t *)rdata));
				memcpy(rdata_with_pref + num_printed,
				       rdata_name, strlen(rdata_name));
				fs_add_uint64(afs, "rdata_is_parsed", 1);
				fs_add_unsafe_string(afs, "rdata",
						     rdata_with_pref, 1);
			}
		}
	} else if (type == DNS_QTYPE_TXT) {
		if (rdlength >= 1 && (rdlength - 1) != *(uint8_t *)rdata) {
			log_warn(
			    "dns",
			    "TXT record with wrong TXT len. Not processing.");
			fs_add_uint64(afs, "rdata_is_parsed", 0);
			fs_add_binary(afs, "rdata", rdlength, rdata, 0);
		} else {
			fs_add_uint64(afs, "rdata_is_parsed", 1);
			char *txt = xmalloc(rdlength);
			memcpy(txt, rdata + 1, rdlength - 1);
			fs_add_unsafe_string(afs, "rdata", txt, 1);
		}
	} else if (type == DNS_QTYPE_A) {
		if (rdlength != 4) {
			log_warn(
			    "dns",
			    "A record with IP of length %d. Not processing.",
			    rdlength);
			fs_add_uint64(afs, "rdata_is_parsed", 0);
			fs_add_binary(afs, "rdata", rdlength, rdata, 0);
		} else {
			fs_add_uint64(afs, "rdata_is_parsed", 1);
			char *addr =
			    strdup(inet_ntoa(*(struct in_addr *)rdata));
			fs_add_unsafe_string(afs, "rdata", addr, 1);
		}
	} else if (type == DNS_QTYPE_AAAA) {
		if (rdlength != 16) {
			log_warn(
			    "dns",
			    "AAAA record with IP of length %d. Not processing.",
			    rdlength);
			fs_add_uint64(afs, "rdata_is_parsed", 0);
			fs_add_binary(afs, "rdata", rdlength, rdata, 0);
		} else {
			fs_add_uint64(afs, "rdata_is_parsed", 1);
			char *ipv6_str = xmalloc(INET6_ADDRSTRLEN);

			inet_ntop(AF_INET6, (struct sockaddr_in6 *)rdata,
				  ipv6_str, INET6_ADDRSTRLEN);

			fs_add_unsafe_string(afs, "rdata", ipv6_str, 1);
		}
	} else {
		fs_add_uint64(afs, "rdata_is_parsed", 0);
		fs_add_binary(afs, "rdata", rdlength, rdata, 0);
	}
	// Now we're adding the new fs to the list.
	fs_add_fieldset(list, NULL, afs);
	// Now update the pointers.
	*data = *data + bytes_consumed + sizeof(dns_answer_tail) + rdlength;
	*data_len =
	    *data_len - bytes_consumed - sizeof(dns_answer_tail) - rdlength;
	log_trace("dns",
		  "return success from process_response_answer, data_len: %d",
		  *data_len);
	return false;
}

/*
 * Start of required zmap exports.
 */

static int dns_global_initialize(struct state_conf *conf)
{
	setup_qtype_str_map();
	if (conf->validate_source_port_override == VALIDATE_SRC_PORT_DISABLE_OVERRIDE) {
		log_debug("dns", "disabling source port validation");
		should_validate_src_port = false;
	}
	if (!conf->probe_args) {
		log_fatal("dns", "Need probe args, e.g. --probe-args=\"A,example.com\"");
	}
	// strip off any leading or trailing semicolons
	if (*conf->probe_args == probe_arg_delimitor[0]) {
		log_debug("dns", "Probe args (%s) contains leading semicolon. Stripping.", conf->probe_args);
		conf->probe_args++;
	}
	if (conf->probe_args[strlen(conf->probe_args) - 1] == probe_arg_delimitor[0]) {
		log_debug("dns", "Probe args (%s) contains trailing semicolon. Stripping.", conf->probe_args);
		conf->probe_args[strlen(conf->probe_args) - 1] = '\0';
	}

	char **domains = NULL;
	num_questions = 0;

	if (conf->probe_args) {
		char *questions_ctx;
		char *domain_ctx;
		char *domain_and_qtype = strtok_r(conf->probe_args, probe_arg_delimitor, &questions_ctx);

		// Process each pair
		while (domain_and_qtype != NULL) {
			// resize the array to accommodate the new pair
			domains = xrealloc(domains, (num_questions + 1) * sizeof(char *));
			qtypes = xrealloc(qtypes, (num_questions + 1) * sizeof(uint16_t));
			rdbits = xrealloc(rdbits, (num_questions + 1) * sizeof(uint8_t));
			rdbits[num_questions] = default_rdbit;

			// Tokenize pair based on comma
			char *qtype_token = strtok_r(domain_and_qtype, domain_qtype_delimitor, &domain_ctx);
			char *domain_token = strtok_r(NULL, domain_qtype_delimitor, &domain_ctx);
			if (strchr(qtype_token, rn_delimitor[0]) != NULL) {
				// need to check if user supplied the no-recursion bit
				char *rbit_ctx;
				char *recurse_token = strtok_r(qtype_token, rn_delimitor, &rbit_ctx);
				recurse_token = strtok_r(NULL, rn_delimitor, &rbit_ctx);
				// check if the no-recursion field matches the expected value ("nr")
				if (strcmp(recurse_token, qopts_rn) == 0) {
					rdbits[num_questions] = 0;
				} else {
					log_warn("dns", "invalid text after DNS query type (%s). no recursion set with \"nr\"", recurse_token);
				}
			}
			if (domain_token == NULL || qtype_token == NULL) {
				log_fatal("dns", "Invalid probe args (%s). Format: \"A,google.com\" "
						 "or \"A,google.com;A,example.com\"",
					  conf->probe_args);
			}
			if (strlen(domain_token) == 0) {
				log_fatal("dns", "Invalid domain, domain cannot be empty.");
			}
			uint domain_len = strlen(domain_token);
			// add space for the null terminator
			char *domain_ptr = xmalloc(domain_len + 1);
			strncpy(domain_ptr, domain_token, domain_len + 1);
			// add null terminator
			domain_ptr[domain_len] = '\0';

			// print debug info
			if (rdbits[num_questions] == 0) {
				// recursion disabled
				log_debug("dns", "parsed user input to scan domain (%s), for qtype (%s) w/o recursion", domain_ptr, qtype_token);
			} else {
				log_debug("dns", "parsed user input to scan domain (%s), for qtype (%s) with recursion", domain_ptr, qtype_token);
			}
			// add the new pair to the array
			domains[num_questions] = domain_ptr;
			qtypes[num_questions] = qtype_str_to_code(qtype_token);

			if (!qtypes[num_questions]) {
				log_fatal("dns", "Incorrect qtype supplied. %s", qtype_token);
			}

			// move to the next pair of domain/qtype
			domain_and_qtype = strtok_r(NULL, probe_arg_delimitor, &questions_ctx);
			num_questions++;
		}
	}

	if (num_questions == 0) {
		// user didn't provide any questions, setting up a default
		log_warn("dns", "no dns questions provided, using default domain (%s) and qtype (%s)", default_domain, qtype_strs[qtype_qtype_to_strid[default_qtype]]);
		// Resize the array to accommodate the new pair
		domains = xrealloc(domains, (num_questions + 1) * sizeof(char *));
		qtypes = xrealloc(qtypes, (num_questions + 1) * sizeof(uint16_t));
		rdbits = xrealloc(rdbits, (num_questions + 1) * sizeof(uint8_t));
		rdbits[num_questions] = default_rdbit;

		// Add the new pair to the array
		domains[num_questions] = strdup(default_domain);
		qtypes[num_questions] = default_qtype;

		num_questions = 1;
	} else {
		log_debug("dns", "number of dns questions: %d", num_questions);
	}

	if (conf->packet_streams % num_questions != 0) {
		// probe count must be a multiple of the number of DNS questions
		log_fatal("dns", "number of probes (%d) must be a multiple of the number of DNS questions (%d)."
				 "Example: '-P 4 --probe-args \"A,google.com;AAAA,cloudflare.com\"'",
			  conf->packet_streams, num_questions);
	}
	// Setup the global structures
	dns_packets = xmalloc(sizeof(char *) * num_questions);
	dns_packet_lens = xmalloc(sizeof(uint16_t) * num_questions);
	qname_lens = xmalloc(sizeof(uint16_t) * num_questions);
	qnames = xmalloc(sizeof(char *) * num_questions);
	num_ports = conf->source_port_last - conf->source_port_first + 1;

	size_t max_payload_len;
	int ret = build_global_dns_packets(domains, num_questions, &max_payload_len);
	module_dns.max_packet_length = max_payload_len + sizeof(struct ether_header) + sizeof(struct ip) + sizeof(struct udphdr);
	return ret;
}

static int dns_global_cleanup(UNUSED struct state_conf *zconf,
			      UNUSED struct state_send *zsend,
			      UNUSED struct state_recv *zrecv)
{
	if (dns_packets) {
		for (int i = 0; i < num_questions; i++) {
			if (dns_packets[i]) {
				free(dns_packets[i]);
			}
		}
		free(dns_packets);
	}
	dns_packets = NULL;

	if (qnames) {
		for (int i = 0; i < num_questions; i++) {
			if (qnames[i]) {
				free(qnames[i]);
			}
		}
		free(qnames);
	}
	qnames = NULL;

	if (dns_packet_lens) {
		free(dns_packet_lens);
	}

	if (qname_lens) {
		free(qname_lens);
	}

	if (qtypes) {
		free(qtypes);
	}

	return EXIT_SUCCESS;
}

int dns_prepare_packet(void *buf, macaddr_t *src, macaddr_t *gw,
		       UNUSED void *arg_ptr)
{
	memset(buf, 0, MAX_PACKET_SIZE);

	// Setup assuming num_questions == 0
	struct ether_header *eth_header = (struct ether_header *)buf;
	make_eth_header(eth_header, src, gw);

	struct ip *ip_header = (struct ip *)(&eth_header[1]);
	uint16_t len = htons(sizeof(struct ip) + sizeof(struct udphdr) +
			     dns_packet_lens[0]);
	make_ip_header(ip_header, IPPROTO_UDP, len);

	struct udphdr *udp_header = (struct udphdr *)(&ip_header[1]);
	len = sizeof(struct udphdr) + dns_packet_lens[0];
	make_udp_header(udp_header, len);

	char *payload = (char *)(&udp_header[1]);

	memcpy(payload, dns_packets[0], dns_packet_lens[0]);

	return EXIT_SUCCESS;
}

// get_dns_question_index_by_probe_num - Find the dns question associated with this probe number
// We allow users to enter a probe count that is a multiple of the number of DNS questions.
// send.c will iterate with this probe count, sending a packet for each probe number
// Ex. -P 4 --probe-args="A,google.com;AAAA,cloudflare.com" - send 2 probes for each question
// Probe_num  |   num_questions   =   dns_index
//      0     |       2           =       0
//      1     |       2           =       1
//      2     |       2           =       0
//      3     |       2           =       1
int get_dns_question_index_by_probe_num(int probe_num)
{
	assert(probe_num >= 0);
	return probe_num % num_questions;
}


// 新增工具函数：将二进制数据转换为可打印ASCII字符串
// added by pqm
static void log_ascii_payload(const char *func, int line, const char *data, size_t len)
{
    const int BYTES_PER_LINE = 64;
    char ascii_buf[BYTES_PER_LINE + 1]; // 改为固定大小数组
    
    if (!data || len == 0) {
        log_error("dns", "%s:%d - Invalid payload pointer", func, line);
        return;
    }

    // 初始化缓冲区
    memset(ascii_buf, 0, sizeof(ascii_buf)); // 代替 = {0} 初始化

    size_t buf_pos = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        
        // 处理可打印字符
        if (byte >= 0x20 && byte <= 0x7E) { // 使用16进制表示更规范
            ascii_buf[buf_pos] = (char)byte;
        } else {
            ascii_buf[buf_pos] = '.'; 
        }
        
        // 行缓冲处理
        if (++buf_pos >= BYTES_PER_LINE) {
            log_debug("dns", "%s:%d - Payload: %s", func, line, ascii_buf);
            memset(ascii_buf, 0, sizeof(ascii_buf));
            buf_pos = 0;
        }
    }
    
    // 处理剩余未满一行的数据
    if (buf_pos > 0) {
        ascii_buf[buf_pos] = '\0'; // 确保字符串终止
        log_debug("dns", "%s:%d - Payload: %s", func, line, ascii_buf);
    }
}

int dns_make_packet(void *buf, size_t *buf_len, ipaddr_n_t src_ip,
		    ipaddr_n_t dst_ip, port_n_t dport, uint8_t ttl,
		    uint32_t *validation, int probe_num,
		    uint16_t ip_id, UNUSED void *arg)
{
	// 输入参数校验, added by pqm

    // if (!buf || !buf_len || *buf_len < sizeof(struct ether_header) + sizeof(struct ip)) {
    //     log_error("dns", "%s:%d - Invalid buffer parameters", __FILE__, __LINE__);
    //     return EXIT_FAILURE;
    // }

	struct ether_header *eth_header = (struct ether_header *)buf;
	struct ip *ip_header = (struct ip *)(&eth_header[1]);
	struct udphdr *udp_header = (struct udphdr *)&ip_header[1];

	
	// added on 2025-03-24 by pqm
	// log_debug("dns", "dns_make_packet, qname: %s",zconf.dnsippadding?"true":"false");


	// For num_questions == 0, we handle this in per-thread init. Do less
	// work
	if (num_questions > 0) {
		int dns_index = get_dns_question_index_by_probe_num(probe_num);
		uint16_t encoded_len =
		    htons(sizeof(struct ip) + sizeof(struct udphdr) +
			  dns_packet_lens[dns_index]);
		make_ip_header(ip_header, IPPROTO_UDP, encoded_len);

		encoded_len =
		    sizeof(struct udphdr) + dns_packet_lens[dns_index];
		make_udp_header(udp_header, encoded_len);

		char *payload = (char *)(&udp_header[1]);
		*buf_len = sizeof(struct ether_header) + sizeof(struct ip) +
			   sizeof(struct udphdr) + dns_packet_lens[dns_index];

		assert(*buf_len <= MAX_PACKET_SIZE);

		memcpy(payload, dns_packets[dns_index],
		       dns_packet_lens[dns_index]);
	}

	ip_header->ip_src.s_addr = src_ip;
	ip_header->ip_dst.s_addr = dst_ip;
	ip_header->ip_ttl = ttl;
	ip_header->ip_id = ip_id;
	// Above we wanted to look up the dns question index (so we could send 2 probes for the same DNS query)
	// Here we want the port to be unique regardless of if this is the 2nd probe to the same DNS query so using
	// probe_num itself to set the unique UDP source port.
	udp_header->uh_sport =
	    htons(get_src_port(num_ports, probe_num, validation));
	udp_header->uh_dport = dport;

	dns_header *dns_header_p = (dns_header *)&udp_header[1];

	dns_header_p->id = validation[2] & 0xFFFF;

	ip_header->ip_sum = 0;
	ip_header->ip_sum = zmap_ip_checksum((unsigned short *)ip_header);

	// added on 2025-03-24 by pqm
	if (zconf.dnsippadding)
	{
		if (*buf_len < 54 + 16) {
            log_error("dns", "%s:%d - Buffer too small for IP padding (need %d, have %zu)",
                     __FILE__, __LINE__, 54+16, *buf_len);
            // return EXIT_FAILURE;
        }
		char *ipqname = make_ip_strinqname(dst_ip);
		// log_debug("dns", "dns_make_packet, qname: %s",ipqname);
		
		memcpy(buf + 54, ipqname, 16);		

		free(ipqname);
	}
	// 日志记录（增加上下文信息）, added by pqm
    if (*buf_len > 54) {
        size_t payload_len = *buf_len - 54;
        log_ascii_payload(__func__, __LINE__, (char*)buf + 54, payload_len);
    } else {
        log_warn("dns", "%s:%d - Packet too small for DNS payload (len=%zu)", 
                __FILE__, __LINE__, *buf_len);
    }

	return EXIT_SUCCESS;
}

void dns_print_packet(FILE *fp, void *packet)
{
	struct ether_header *ethh = (struct ether_header *)packet;
	struct ip *iph = (struct ip *)&ethh[1];
	struct udphdr *udph = (struct udphdr *)(&iph[1]);
	fprintf(fp, PRINT_PACKET_SEP);
	fprintf(fp, "dns { source: %u | dest: %u | checksum: %#04X }\n",
		ntohs(udph->uh_sport), ntohs(udph->uh_dport),
		ntohs(udph->uh_sum));
	fprintf_ip_header(fp, iph);
	fprintf_eth_header(fp, ethh);
	fprintf(fp, PRINT_PACKET_SEP);
}

int dns_validate_packet(const struct ip *ip_hdr, uint32_t len, uint32_t *src_ip,
			uint32_t *validation, const struct port_conf *ports)
{
	// this does the heavy lifting including ICMP validation
	if (udp_do_validate_packet(ip_hdr, len, src_ip, validation, num_ports, should_validate_src_port, ports) == PACKET_INVALID) {
		return PACKET_INVALID;
	}
	if (ip_hdr->ip_p == IPPROTO_UDP) {
		struct udphdr *udp = get_udp_header(ip_hdr, len);
		if (!udp) {
			return PACKET_INVALID;
		}
		// verify our packet length
		uint16_t udp_len = ntohs(udp->uh_ulen);
		int match = 0;
		for (int i = 0; i < num_questions; i++) {
			if (udp_len >= dns_packet_lens[i]) {
				match += 1;
			}
		}
		if (match == 0) {
			return PACKET_INVALID;
		}
		if (len < udp_len) {
			return PACKET_INVALID;
		}
	}
	return PACKET_VALID;
}

void dns_add_null_fs(fieldset_t *fs)
{
	fs_add_null(fs, "dns_id");
	fs_add_null(fs, "dns_rd");
	fs_add_null(fs, "dns_tc");
	fs_add_null(fs, "dns_aa");
	fs_add_null(fs, "dns_opcode");
	fs_add_null(fs, "dns_qr");
	fs_add_null(fs, "dns_rcode");
	fs_add_null(fs, "dns_cd");
	fs_add_null(fs, "dns_ad");
	fs_add_null(fs, "dns_z");
	fs_add_null(fs, "dns_ra");
	fs_add_null(fs, "dns_qdcount");
	fs_add_null(fs, "dns_ancount");
	fs_add_null(fs, "dns_nscount");
	fs_add_null(fs, "dns_arcount");

	fs_add_repeated(fs, "dns_questions", fs_new_repeated_fieldset());
	fs_add_repeated(fs, "dns_answers", fs_new_repeated_fieldset());
	fs_add_repeated(fs, "dns_authorities", fs_new_repeated_fieldset());
	fs_add_repeated(fs, "dns_additionals", fs_new_repeated_fieldset());

	fs_add_uint64(fs, "dns_parse_err", 1);
	fs_add_uint64(fs, "dns_unconsumed_bytes", 0);
}

void dns_process_packet(const u_char *packet, uint32_t len, fieldset_t *fs,
			uint32_t *validation,
			UNUSED struct timespec ts)
{
	struct ip *ip_hdr = (struct ip *)&packet[sizeof(struct ether_header)];
	if (ip_hdr->ip_p == IPPROTO_UDP) {
		struct udphdr *udp_hdr = get_udp_header(ip_hdr, len);
		assert(udp_hdr);
		uint16_t udp_len = ntohs(udp_hdr->uh_ulen);

		int match = 0;
		bool is_valid = false;
		for (int i = 0; i < num_questions; i++) {
			if (udp_len < dns_packet_lens[i]) {
				continue;
			}
			match += 1;

			char *qname_p = NULL;
			dns_question_tail *tail_p = NULL;
			dns_header *dns_header_p = (dns_header *)&udp_hdr[1];
			// verify our dns transaction id
			if (dns_header_p->id == (validation[2] & 0xFFFF)) {
				// Verify our question
				qname_p =
				    (char *)dns_header_p + sizeof(dns_header);
				tail_p =
				    (dns_question_tail *)(dns_packets[i] +
							  sizeof(dns_header) +
							  qname_lens[i]);
				// Verify our qname
				// added by pqm
				int offset = zconf.dnsippadding ? 16:0;
				if (strcmp(qnames[i] + offset, qname_p + offset) == 0) {
					// Verify the qtype and qclass.
					if (tail_p->qtype == htons(qtypes[i]) &&
					    tail_p->qclass == htons(0x01)) {
						is_valid = true;
						break;
					}
				}
			}
		}
		assert(match > 0);

		dns_header *dns_hdr = (dns_header *)&udp_hdr[1];
		uint16_t qr = dns_hdr->qr;
		uint16_t rcode = dns_hdr->rcode;
		// Success: Has the right validation bits and the right Q
		// App success: has qr and rcode bits right
		// Any app level parsing issues: dns_parse_err
		//
		fs_add_uint64(fs, "sport", ntohs(udp_hdr->uh_sport));
		fs_add_uint64(fs, "dport", ntohs(udp_hdr->uh_dport));

		// High level info
		fs_add_string(fs, "classification", (char *)"dns", 0);
		fs_add_bool(fs, "success", is_valid);
		// additional UDP information
		fs_add_bool(fs, "app_success",
			    is_valid && (qr == DNS_QR_ANSWER) &&
				(rcode == DNS_RCODE_NOERR));
		// ICMP info
		fs_add_null_icmp(fs);
		fs_add_uint64(fs, "udp_len", udp_len);
		// DNS data
		if (!is_valid) {
			dns_add_null_fs(fs);
		} else {
			// DNS header
			fs_add_uint64(fs, "dns_id", ntohs(dns_hdr->id));
			fs_add_uint64(fs, "dns_rd", dns_hdr->rd);
			fs_add_uint64(fs, "dns_tc", dns_hdr->tc);
			fs_add_uint64(fs, "dns_aa", dns_hdr->aa);
			fs_add_uint64(fs, "dns_opcode", dns_hdr->opcode);
			fs_add_uint64(fs, "dns_qr", qr);
			fs_add_uint64(fs, "dns_rcode", rcode);
			fs_add_uint64(fs, "dns_cd", dns_hdr->cd);
			fs_add_uint64(fs, "dns_ad", dns_hdr->ad);
			fs_add_uint64(fs, "dns_z", dns_hdr->z);
			fs_add_uint64(fs, "dns_ra", dns_hdr->ra);
			fs_add_uint64(fs, "dns_qdcount",
				      ntohs(dns_hdr->qdcount));
			fs_add_uint64(fs, "dns_ancount",
				      ntohs(dns_hdr->ancount));
			fs_add_uint64(fs, "dns_nscount",
				      ntohs(dns_hdr->nscount));
			fs_add_uint64(fs, "dns_arcount",
				      ntohs(dns_hdr->arcount));
			// And now for the complicated part. Hierarchical data.
			char *data = ((char *)dns_hdr) + sizeof(dns_header);
			uint16_t data_len =
			    udp_len - sizeof(udp_hdr) - sizeof(dns_header);
			bool err = false;
			// Questions
			fieldset_t *list = fs_new_repeated_fieldset();
			for (int i = 0; i < ntohs(dns_hdr->qdcount) && !err;
			     i++) {
				err = process_response_question(
				    &data, &data_len, (char *)dns_hdr, udp_len,
				    list);
			}
			fs_add_repeated(fs, "dns_questions", list);
			// Answers
			list = fs_new_repeated_fieldset();
			for (int i = 0; i < ntohs(dns_hdr->ancount) && !err;
			     i++) {
				err = process_response_answer(&data, &data_len,
							      (char *)dns_hdr,
							      udp_len, list);
			}
			fs_add_repeated(fs, "dns_answers", list);
			// Authorities
			list = fs_new_repeated_fieldset();
			for (int i = 0; i < ntohs(dns_hdr->nscount) && !err;
			     i++) {
				err = process_response_answer(&data, &data_len,
							      (char *)dns_hdr,
							      udp_len, list);
			}
			fs_add_repeated(fs, "dns_authorities", list);
			// Additionals
			list = fs_new_repeated_fieldset();
			for (int i = 0; i < ntohs(dns_hdr->arcount) && !err;
			     i++) {
				err = process_response_answer(&data, &data_len,
							      (char *)dns_hdr,
							      udp_len, list);
			}
			fs_add_repeated(fs, "dns_additionals", list);
			// Do we have unconsumed data?
			if (data_len != 0) {
				err = true;
			}
			// Did we parse OK?
			fs_add_uint64(fs, "dns_parse_err", err);
			fs_add_uint64(fs, "dns_unconsumed_bytes", data_len);
		}
		// Now the raw stuff.
		fs_add_binary(fs, "raw_data", (udp_len - sizeof(struct udphdr)),
			      (void *)&udp_hdr[1], 0);
	} else if (ip_hdr->ip_p == IPPROTO_ICMP) {
		fs_add_null(fs, "sport");
		fs_add_null(fs, "dport");
		fs_add_constchar(fs, "classification", "icmp");
		fs_add_bool(fs, "success", 0);
		fs_add_bool(fs, "app_success", 0);
		// Populate all ICMP Fields
		fs_populate_icmp_from_iphdr(ip_hdr, len, fs);
		fs_add_null(fs, "udp_len");
		dns_add_null_fs(fs);
		fs_add_binary(fs, "raw_data", len, (char *)packet, 0);
	} else {
		// This should not happen. Both the pcap filter and validate
		// packet prevent this.
		log_fatal("dns", "Die. This can only happen if you "
				 "change the pcap filter and don't update the "
				 "process function.");
	}
}

static fielddef_t fields[] = {
    {.name = "sport", .type = "int", .desc = "UDP source port"},
    {.name = "dport", .type = "int", .desc = "UDP destination port"},
    CLASSIFICATION_SUCCESS_FIELDSET_FIELDS,
    {.name = "app_success",
     .type = "bool",
     .desc = "Is the RA bit set with no error code?"},
    ICMP_FIELDSET_FIELDS,
    {.name = "udp_len", .type = "int", .desc = "UDP packet length"},
    {.name = "dns_id", .type = "int", .desc = "DNS transaction ID"},
    {.name = "dns_rd", .type = "int", .desc = "DNS recursion desired"},
    {.name = "dns_tc", .type = "int", .desc = "DNS packet truncated"},
    {.name = "dns_aa", .type = "int", .desc = "DNS authoritative answer"},
    {.name = "dns_opcode", .type = "int", .desc = "DNS opcode (query type)"},
    {.name = "dns_qr", .type = "int", .desc = "DNS query(0) or response (1)"},
    {.name = "dns_rcode", .type = "int", .desc = "DNS response code"},
    {.name = "dns_cd", .type = "int", .desc = "DNS checking disabled"},
    {.name = "dns_ad", .type = "int", .desc = "DNS authenticated data"},
    {.name = "dns_z", .type = "int", .desc = "DNS reserved"},
    {.name = "dns_ra", .type = "int", .desc = "DNS recursion available"},
    {.name = "dns_qdcount", .type = "int", .desc = "DNS number questions"},
    {.name = "dns_ancount", .type = "int", .desc = "DNS number answer RR's"},
    {.name = "dns_nscount",
     .type = "int",
     .desc = "DNS number NS RR's in authority section"},
    {.name = "dns_arcount",
     .type = "int",
     .desc = "DNS number additional RR's"},
    {.name = "dns_questions", .type = "repeated", .desc = "DNS question list"},
    {.name = "dns_answers", .type = "repeated", .desc = "DNS answer list"},
    {.name = "dns_authorities",
     .type = "repeated",
     .desc = "DNS authority list"},
    {.name = "dns_additionals",
     .type = "repeated",
     .desc = "DNS additional list"},
    {.name = "dns_parse_err",
     .type = "int",
     .desc = "Problem parsing the DNS response"},
    {.name = "dns_unconsumed_bytes",
     .type = "int",
     .desc = "Bytes left over when parsing"
	     " the DNS response"},
    {.name = "raw_data", .type = "binary", .desc = "UDP payload"},
};

probe_module_t module_dns = {
    .name = "dns",
    .max_packet_length = 0, // set in init
    .pcap_filter = "udp || icmp",
    .pcap_snaplen = PCAP_SNAPLEN,
    .port_args = 1,
    .global_initialize = &dns_global_initialize,
    .prepare_packet = &dns_prepare_packet,
    .make_packet = &dns_make_packet,
    .print_packet = &dns_print_packet,
    .validate_packet = &dns_validate_packet,
    .process_packet = &dns_process_packet,
    .close = &dns_global_cleanup,
    .output_type = OUTPUT_TYPE_DYNAMIC,
    .fields = fields,
    .numfields = sizeof(fields) / sizeof(fields[0]),
    .helptext =
	"This module sends out DNS queries and parses basic responses. "
	"By default, the module will perform an A record lookup for "
	"google.com. You can specify other queries using the --probe-args "
	"argument in the form: 'type,query', e.g. 'A,google.com'. The --probes/-P "
	"flag must be set to a multiple of the number of DNS questions. The module "
	"supports sending the the following types of queries: A, NS, CNAME, SOA, "
	"PTR, MX, TXT, AAAA, RRSIG, and ALL. In order to send queries with the "
	"'recursion desired' bit set to 0, append the suffix ':nr' to the query "
	"type, e.g. 'A:nr,google.com'. The module will accept and attempt "
	"to parse all DNS responses. There is currently support for parsing out "
	"full data from A, NS, CNAME, MX, TXT, and AAAA. Any other types will be "
	"output in raw form."

};
