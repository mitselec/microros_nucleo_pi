/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal UDP transport for micro-ROS on Zephyr.
 *
 * This replaces the transport shipped in micro_ros_zephyr_module, which:
 *   - resolves the agent address with getaddrinfo(), which needs a heap
 *     (and is pointless for a numeric IPv4 address);
 *   - leaks the socket descriptor whenever socket()/connect() fails, so
 *     the ping-retry loop can exhaust the fd table before DHCP finishes;
 *   - prints nothing when it fails, leaving the console stuck on
 *     "Waiting for the micro-ROS agent...".
 */

#ifndef APP_UDP_TRANSPORT_H_
#define APP_UDP_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uxr/client/profile/transport/custom/custom_transport.h>

typedef struct {
	char ip[16];
	char port[6];
	int fd;
	/* Set after the first successful open, so the reconnect loop does not
	 * print a socket line every time it probes for the agent.
	 */
	bool open_logged;
} app_udp_params_t;

bool app_udp_open(struct uxrCustomTransport *transport);
bool app_udp_close(struct uxrCustomTransport *transport);
size_t app_udp_write(struct uxrCustomTransport *transport,
		     const uint8_t *buf, size_t len, uint8_t *err);
size_t app_udp_read(struct uxrCustomTransport *transport,
		    uint8_t *buf, size_t len, int timeout, uint8_t *err);

#endif /* APP_UDP_TRANSPORT_H_ */
