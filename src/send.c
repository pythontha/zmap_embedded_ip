/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include "send.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>

#include "../lib/includes.h"
#include "../lib/util.h"
#include "../lib/logger.h"
#include "../lib/random.h"
#include "../lib/blocklist.h"
#include "../lib/lockfd.h"
#include "../lib/pbm.h"
#include "../lib/xalloc.h"

#include "send-internal.h"
#include "aesrand.h"
#include "get_gateway.h"
#include "iterator.h"
#include "probe_modules/packet.h"
#include "probe_modules/probe_modules.h"
#include "shard.h"
#include "state.h"
#include "validate.h"
#include "ipv6_target_file.h"

// The iterator over the cyclic group

// Lock for send run
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

// Source ports for outgoing packets
static uint16_t num_src_ports;

// IPv6
static int ipv6 = 0;
static struct in6_addr ipv6_src;


void sig_handler_increase_speed(UNUSED int signal)
{
	int old_rate = zconf.rate;
	zconf.rate += (zconf.rate * 0.05);
	log_info("send", "send rate increased from %i to %i pps.", old_rate,
		 zconf.rate);
}

void sig_handler_decrease_speed(UNUSED int signal)
{
	int old_rate = zconf.rate;
	zconf.rate -= (zconf.rate * 0.05);
	log_info("send", "send rate decreased from %i to %i pps.", old_rate,
		 zconf.rate);
}

// global sender initialize (not thread specific)
iterator_t *send_init(void)
{
	// IPv6
	if (zconf.ipv6_target_filename) {
		ipv6 = 1;
		int ret = inet_pton(AF_INET6, (char *) zconf.ipv6_source_ip, &ipv6_src);
		if (ret != 1) {
			log_fatal("send", "could not read valid IPv6 src address, inet_pton returned `%d'", ret);
		}
		ipv6_target_file_init(zconf.ipv6_target_filename);
	}

	// generate a new primitive root and starting position
	iterator_t *it;
	uint32_t num_subshards = (uint32_t)zconf.senders * (uint32_t)zconf.total_shards;
	if (num_subshards > (blocklist_count_allowed() * zconf.ports->port_count)) {
		log_fatal("send", "senders * shards > allowed probes");
	}
	if (zsend.max_targets && (num_subshards > zsend.max_targets)) {
		log_fatal("send", "senders * shards > max targets");
	}
	uint64_t num_addrs = blocklist_count_allowed();
	it = iterator_init(zconf.senders, zconf.shard_num, zconf.total_shards,
			   num_addrs, zconf.ports->port_count);
	// determine the source address offset from which we'll send packets
	struct in_addr temp;
	temp.s_addr = zconf.source_ip_addresses[0];
	log_debug("send", "srcip_first: %s", inet_ntoa(temp));
	temp.s_addr = zconf.source_ip_addresses[zconf.number_source_ips - 1];
	log_debug("send", "srcip_last: %s", inet_ntoa(temp));

	// process the source port range that ZMap is allowed to use
	num_src_ports = zconf.source_port_last - zconf.source_port_first + 1;
	log_debug("send", "will send from %u address%s on %hu source ports",
		  zconf.number_source_ips,
		  ((zconf.number_source_ips == 1) ? "" : "es"), num_src_ports);
	// global initialization for send module
	assert(zconf.probe_module);
	if (zconf.probe_module->global_initialize) {
		if (zconf.probe_module->global_initialize(&zconf)) {
			log_fatal(
			    "send",
			    "global initialization for probe module failed.");
		}
	}
	// only allow bandwidth or rate
	if (zconf.bandwidth > 0 && zconf.rate > 0) {
		log_fatal(
		    "send",
		    "must specify rate or bandwidth, or neither, not both.");
	}
	// Convert specified bandwidth to packet rate. This is an estimate using the
	// max packet size a probe module will generate.
	if (zconf.bandwidth > 0) {
		size_t pkt_len = zconf.probe_module->max_packet_length;
		pkt_len *= 8;
		// 7 byte MAC preamble, 1 byte Start frame, 4 byte CRC, 12 byte
		// inter-frame gap
		pkt_len += 8 * 24;
		// adjust calculated length if less than the minimum size of an
		// ethernet frame
		if (pkt_len < 84 * 8) {
			pkt_len = 84 * 8;
		}
		// rate is a uint32_t so, don't overflow
		if (zconf.bandwidth / pkt_len > 0xFFFFFFFFu) {
			zconf.rate = 0;
		} else {
			zconf.rate = zconf.bandwidth / pkt_len;
			if (zconf.rate == 0) {
				log_warn(
				    "send",
				    "bandwidth %lu bit/s is slower than 1 pkt/s, "
				    "setting rate to 1 pkt/s",
				    zconf.bandwidth);
				zconf.rate = 1;
			}
		}
		log_debug(
		    "send",
		    "using bandwidth %lu bits/s for %zu byte probe, rate set to %d pkt/s",
		    zconf.bandwidth, pkt_len / 8, zconf.rate);
	}
	// convert default placeholder to default value
	if (zconf.rate == -1) {
		// default 10K pps
		zconf.rate = 10000;
	}
	// log rate, if explicitly specified
	if (zconf.rate < 0) {
		log_fatal("send", "rate impossibly slow");
	}
	if (zconf.rate > 0 && zconf.bandwidth <= 0) {
		log_debug("send", "rate set to %d pkt/s", zconf.rate);
	}
	// Get the source hardware address, and give it to the probe
	// module
	if (!zconf.hw_mac_set) {
		if (get_iface_hw_addr(zconf.iface, zconf.hw_mac)) {
			log_fatal(
			    "send",
			    "ZMap could not retrieve the hardware (MAC) address for "
			    "the interface \"%s\". You likely do not privileges to open a raw packet socket. "
			    "Are you running as root or with the CAP_NET_RAW capability? If you are, you "
			    "may need to manually set the source MAC address with the \"--source-mac\" flag.",
			    zconf.iface);
			return NULL;
		}
		log_debug(
		    "send",
		    "no source MAC provided. "
		    "automatically detected %02x:%02x:%02x:%02x:%02x:%02x as hw "
		    "interface for %s",
		    zconf.hw_mac[0], zconf.hw_mac[1], zconf.hw_mac[2],
		    zconf.hw_mac[3], zconf.hw_mac[4], zconf.hw_mac[5],
		    zconf.iface);
	}
	log_debug("send", "source MAC address %02x:%02x:%02x:%02x:%02x:%02x",
		  zconf.hw_mac[0], zconf.hw_mac[1], zconf.hw_mac[2],
		  zconf.hw_mac[3], zconf.hw_mac[4], zconf.hw_mac[5]);

	if (zconf.dryrun) {
		log_info("send", "dryrun mode -- won't actually send packets");
	}
	// initialize random validation key
	validate_init();
	// setup signal handlers for changing scan speed
	signal(SIGUSR1, sig_handler_increase_speed);
	signal(SIGUSR2, sig_handler_decrease_speed);
	zsend.start = now();
	return it;
}

static inline ipaddr_n_t get_src_ip(ipaddr_n_t dst, int local_offset)
{
	if (zconf.number_source_ips == 1) {
		return zconf.source_ip_addresses[0];
	}
	return zconf.source_ip_addresses[(ntohl(dst) + local_offset) %
					 zconf.number_source_ips];
}

// one sender thread
int send_run(sock_t st, shard_t *s)
{
	log_debug("send", "send thread started");
	pthread_mutex_lock(&send_mutex);
	// allocate batch
	batch_t *batch = create_packet_batch(zconf.batch);

	// OS specific per-thread init
	if (send_run_init(st)) {
		pthread_mutex_unlock(&send_mutex);
		return EXIT_FAILURE;
	}

	// MAC address length in characters
	char mac_buf[(ETHER_ADDR_LEN * 2) + (ETHER_ADDR_LEN - 1) + 1];
	char *p = mac_buf;
	for (int i = 0; i < ETHER_ADDR_LEN; i++) {
		if (i == ETHER_ADDR_LEN - 1) {
			snprintf(p, 3, "%.2x", zconf.hw_mac[i]);
			p += 2;
		} else {
			snprintf(p, 4, "%.2x:", zconf.hw_mac[i]);
			p += 3;
		}
	}
	log_debug("send", "source MAC address %s", mac_buf);

	void *probe_data = NULL;
	if (zconf.probe_module->thread_initialize) {
		int rv = zconf.probe_module->thread_initialize(&probe_data);
		if (rv != EXIT_SUCCESS) {
			pthread_mutex_unlock(&send_mutex);
			log_fatal("send", "Send thread initialization for probe module failed: %u", rv);
		}
	}
	pthread_mutex_unlock(&send_mutex);

	if (zconf.probe_module->prepare_packet) {
		for (size_t i = 0; i < batch->capacity; i++) {
			int rv = zconf.probe_module->prepare_packet(
			    batch->packets[i].buf, zconf.hw_mac, zconf.gw_mac, probe_data);
			if (rv != EXIT_SUCCESS) {
				log_fatal("send", "Probe module failed to prepare packet: %u", rv);
			}
		}
	}

	
	// adaptive timing to hit target rate
	uint64_t count = 0;
	uint64_t last_count = count;
	double last_time = steady_now();
	uint32_t delay = 0;
	int interval = 0;
	volatile int vi;
	struct timespec ts, rem;
	double send_rate =
	    (double)zconf.rate /
	    ((double)zconf.senders * zconf.packet_streams);
	const double slow_rate = 1000; // packets per seconds per thread
	// at which it uses the slow methods
	long nsec_per_sec = 1000 * 1000 * 1000;
	long long sleep_time = nsec_per_sec;
	if (zconf.rate > 0) {
		delay = 10000;
		if (send_rate < slow_rate) {
			// set the initial time difference
			sleep_time = nsec_per_sec / send_rate;
			last_time = steady_now() - (1.0 / send_rate);
		} else {
			// estimate initial rate
			for (vi = delay; vi--;)
				;
			delay *= 1 / (steady_now() - last_time) /
				 ((double)zconf.rate /
				  (double)zconf.senders);
			interval = ((double)zconf.rate /
				    (double)zconf.senders) /
				   20;
			last_time = steady_now();
			assert(interval > 0);
			if (delay == 0) {
				// at extremely high bandwidths, the delay could be set to zero.
				// this breaks the multiplier logic below, so we'll hard-set it to 1 in this case.
				delay = 1;
			}
		}
	}
	int attempts = zconf.retries + 1;
	// Get the initial IP to scan.
	target_t current;
	uint32_t current_ip = 0;
	uint16_t current_port = 0;
	struct in6_addr ipv6_dst;

	if (ipv6) {
		int ret = ipv6_target_file_get_ipv6(&ipv6_dst);
		if (ret != 0) {
			log_debug("send", "send thread %hhu finished, no more target IPv6 addresses", s->thread_id);
			goto cleanup;
		}
		probe_data = malloc(2*sizeof(struct in6_addr));
		current_port = zconf.ports->ports[0];
	} else {
		current = shard_get_cur_target(s);
		// If provided a list of IPs to scan, then the first generated address
		// might not be on that list. Iterate until the current IP is one the
		// list, then start the true scanning process.
			if (zconf.list_of_ips_filename) {
			while (!pbm_check(zsend.list_of_ips_pbm, current_ip)) {
				current = shard_get_next_target(s);
				current_ip = current.ip;
				current_port = current.port;
				if (current.status == ZMAP_SHARD_DONE) {
					log_debug(
					    "send",
					    "never made it to send loop in send thread %i",
					    s->thread_id);
					goto cleanup;
				}
			}
		}
	}
	while (1) {
		// Adaptive timing delay
		if (count && delay > 0) {
			if (send_rate < slow_rate) {
				double t = steady_now();
				double last_rate = (1.0 / (t - last_time));

				sleep_time *= ((last_rate / send_rate) + 1) / 2;
				ts.tv_sec = sleep_time / nsec_per_sec;
				ts.tv_nsec = sleep_time % nsec_per_sec;
				log_debug("sleep",
					  "sleep for %d sec, %ld nanoseconds",
					  ts.tv_sec, ts.tv_nsec);
				while (nanosleep(&ts, &rem) == -1) {
				}
				last_time = t;
			} else {
				for (vi = delay; vi--;)
					;
				if (!interval || (count % interval == 0)) {
					double t = steady_now();
					assert(count > last_count);
					assert(t > last_time);
					double multiplier =
					    (double)(count - last_count) /
					    (t - last_time) /
					    (zconf.rate / zconf.senders);
					uint32_t old_delay = delay;
					delay *= multiplier;
					if (delay == old_delay) {
						if (multiplier > 1.0) {
							delay *= 2;
						} else if (multiplier < 1.0) {
							delay *= 0.5;
						}
					}
					if (delay == 0) {
						// delay could become zero if the actual send rate stays below the target rate for long enough
						// this could be due to things like VM cpu contention, or the NIC being saturated.
						// However, we never want delay to become 0, as this would remove any rate limiting for the rest
						// of the ZMap invocation (since 0 * multiplier = 0 for any multiplier). To prevent the removal
						// of rate-limiting we'll set delay to one here.
						delay = 1;
					}
					last_count = count;
					last_time = t;
				}
			}
		}

		// Check if the program has otherwise completed and break out of the send loop.
		if (zrecv.complete) {
			goto cleanup;
		}
		if (zconf.max_runtime &&
		    zconf.max_runtime <= now() - zsend.start) {
			goto cleanup;
		}

		// Check if we've finished this shard or thread before sending each
		// packet, regardless of batch size.
		if (s->state.max_targets &&
		    s->state.targets_scanned >= s->state.max_targets) {
			log_debug(
			    "send",
			    "send thread %hhu finished (max targets of %u reached)",
			    s->thread_id, s->state.max_targets);
			goto cleanup;
		}
		if (s->state.max_packets &&
		    s->state.packets_sent >= s->state.max_packets) {
			log_debug(
			    "send",
			    "send thread %hhu finished (max packets of %u reached)",
			    s->thread_id, s->state.max_packets);
			goto cleanup;
		}
		if (!ipv6 && current.status == ZMAP_SHARD_DONE) {
			log_debug(
			    "send",
			    "send thread %hhu finished, shard depleted",
			    s->thread_id);
			goto cleanup;
		}
		for (int i = 0; i < zconf.packet_streams; i++) {
			count++;
			uint32_t src_ip = get_src_ip(current_ip, i);
			uint8_t size_of_validation = VALIDATE_BYTES / sizeof(uint32_t);
			uint32_t validation[size_of_validation];
			// IPv6
			if (ipv6) {
				((struct in6_addr *) probe_data)[0] = ipv6_src;
				((struct in6_addr *) probe_data)[1] = ipv6_dst;
				validate_gen_ipv6(&ipv6_src, &ipv6_dst,
				 					htons(current_port),
					 				(uint8_t *)validation);
			} else {
				validate_gen(src_ip, current_ip,
				 			htons(current_port),
							 (uint8_t *)validation);
			}
			uint8_t ttl = zconf.probe_ttl;
			size_t length = 0;
			zconf.probe_module->make_packet(
			    batch->packets[batch->len].buf, &length,
				src_ip, current_ip, htons(current_port), ttl, validation, i,
				// Grab last 2 bytes of validation for ip_id
			    (uint16_t)(validation[size_of_validation - 1] & 0xFFFF),
			    probe_data);
			if (length > MAX_PACKET_SIZE) {
				log_fatal(
				    "send",
				    "send thread %hhu set length (%zu) larger than MAX (%zu)",
				    s->thread_id, length,
				    MAX_PACKET_SIZE);
			}
			batch->packets[batch->len].len = (uint32_t)length;
			if (zconf.dryrun) {
				batch->len++;
				if (batch->len == batch->capacity) {
					lock_file(stdout);
					for (int i = 0; i < batch->len; i++) {
						zconf.probe_module->print_packet(stdout,
														 batch->packets[i].buf);
					}
					unlock_file(stdout);
					// reset batch length for next batch
					batch->len = 0;
				}
			} else {
                batch->len++;
                if (batch->len == batch->capacity) {
                    // batch is full, sending
                    int rc = send_batch(st, batch, attempts);
                    // whether batch succeeds or fails, this was the only attempt. Any re-tries are     handled within batch
                    if (rc < 0) {
                        log_error("send_batch", "could not send any batch packets: %s", strerror(errno));
                        // rc is the last error code if all packets couldn't be sent
                        s->state.packets_failed += batch->len;
                    } else {
                        // rc is number of packets sent successfully, if > 0
                        s->state.packets_failed += batch->len - rc;
                    }
                    // reset batch length for next batch
                    batch->len = 0;
                }
            }
            s->state.packets_sent++;
        }
        // Track the number of targets (ip,p
		s->state.targets_scanned++;

		// IPv6
		if (ipv6) {
			int ret = ipv6_target_file_get_ipv6(&ipv6_dst);
			if (ret != 0) {
				log_debug("send", "send thread %hhu finished, no more target IPv6 addresses", s->thread_id);
				goto cleanup;
			}
		} else {
			// Get the next IP to scan
			current = shard_get_next_target(s);
			current_ip = current.ip;
			current_port = current.port;
			if (zconf.list_of_ips_filename &&
				current.status != ZMAP_SHARD_DONE) {
				// If we have a list of IPs bitmap, ensure the next IP
				// to scan is on the list.
				while (!pbm_check(zsend.list_of_ips_pbm,
						current_ip)) {
					current = shard_get_next_target(s);
					current_ip = current.ip;
					if (current.status == ZMAP_SHARD_DONE) {
						log_debug(
							"send",
							"send thread %hhu shard finished in get_next_ip_loop depleted",
							s->thread_id);
						goto cleanup;
					}
				}
			}
		}
	}
cleanup:
	if (!zconf.dryrun && send_batch(st, batch, attempts) < 0) {
		log_error("send_batch cleanup", "could not send remaining batch packets: %s", strerror(errno));
	} else if (zconf.dryrun) {
		lock_file(stdout);
		for (int i = 0; i < batch->len; i++) {
			zconf.probe_module->print_packet(stdout,
											 batch->packets[i].buf);
		}
		unlock_file(stdout);
		// reset batch length for next batch
		batch->len = 0;
	}
	free_packet_batch(batch);
	s->cb(s->thread_id, s->arg);
	if (zconf.dryrun) {
		lock_file(stdout);
		fflush(stdout);
		unlock_file(stdout);
	}
	log_debug("send", "thread %hu cleanly finished", s->thread_id);
	return EXIT_SUCCESS;
}

batch_t *create_packet_batch(uint16_t capacity)
{
	// allocating batch and associated data structures in single xmalloc for cache locality
	batch_t *batch = (batch_t *)xmalloc(sizeof(batch_t) + capacity * sizeof(struct batch_packet));
	batch->packets = (struct batch_packet *)(batch + 1);
	batch->capacity = capacity;
	batch->len = 0;
	return batch;
}

void free_packet_batch(batch_t *batch)
{
	// batch was created with a single xmalloc, so this will free the component array too
	xfree(batch);
}
