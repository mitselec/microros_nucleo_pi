/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal UDP transport for micro-ROS on Zephyr.
 *
 * The socket is created once per transport open and always closed on any
 * failure path, so repeated ping attempts cannot exhaust the fd table.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "udp_transport.h"

bool app_udp_open(struct uxrCustomTransport *transport)
{
	app_udp_params_t *params = (app_udp_params_t *)transport->args;
	struct sockaddr_in agent = {0};

	if (params->fd >= 0) {
		return true;
	}

	params->fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (params->fd < 0) {
		printk("udp: socket() failed (errno %d)\n", errno);
		return false;
	}

	agent.sin_family = AF_INET;
	agent.sin_port = htons((uint16_t)strtoul(params->port, NULL, 10));

	if (zsock_inet_pton(AF_INET, params->ip, &agent.sin_addr) != 1) {
		printk("udp: invalid agent address '%s'\n", params->ip);
		goto fail;
	}

	if (zsock_connect(params->fd, (struct sockaddr *)&agent,
			  sizeof(agent)) < 0) {
		printk("udp: connect() to %s:%s failed (errno %d)\n",
		       params->ip, params->port, errno);
		goto fail;
	}

	printk("udp: socket %d connected to %s:%s\n",
	       params->fd, params->ip, params->port);
	return true;

fail:
	(void)zsock_close(params->fd);
	params->fd = -1;
	return false;
}

bool app_udp_close(struct uxrCustomTransport *transport)
{
	app_udp_params_t *params = (app_udp_params_t *)transport->args;
	int fd = params->fd;

	if (fd < 0) {
		return true;
	}

	params->fd = -1;
	return zsock_close(fd) == 0;
}

size_t app_udp_write(struct uxrCustomTransport *transport,
		     const uint8_t *buf, size_t len, uint8_t *err)
{
	app_udp_params_t *params = (app_udp_params_t *)transport->args;
	ssize_t sent;

	if (params->fd < 0) {
		*err = 1;
		return 0;
	}

	sent = zsock_send(params->fd, buf, len, 0);
	if (sent < 0) {
		printk("udp: send() failed (errno %d)\n", errno);
		*err = 1;
		return 0;
	}

	*err = 0;
	return (size_t)sent;
}

size_t app_udp_read(struct uxrCustomTransport *transport,
		    uint8_t *buf, size_t len, int timeout, uint8_t *err)
{
	app_udp_params_t *params = (app_udp_params_t *)transport->args;
	struct zsock_pollfd pfd;
	ssize_t received;
	int ready;

	if (params->fd < 0) {
		*err = 1;
		return 0;
	}

	pfd.fd = params->fd;
	pfd.events = ZSOCK_POLLIN;
	pfd.revents = 0;

	ready = zsock_poll(&pfd, 1, timeout);
	if (ready == 0) {
		/* Timeout is not an error: the caller decides what to do. */
		*err = 0;
		return 0;
	}
	if (ready < 0) {
		printk("udp: poll() failed (errno %d)\n", errno);
		*err = 1;
		return 0;
	}

	received = zsock_recv(params->fd, buf, len, 0);
	if (received < 0) {
		printk("udp: recv() failed (errno %d)\n", errno);
		*err = 1;
		return 0;
	}

	*err = 0;
	return (size_t)received;
}
