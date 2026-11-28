/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * micro-ROS demo application for the NUCLEO-F767ZI board on Zephyr.
 *
 * Transport: UDP over the integrated Ethernet (module transport
 * CONFIG_MICROROS_TRANSPORT_UDP), connecting to a micro-ROS agent
 * (micro-ROS Docker agent) on the Raspberry Pi.
 *
 * Behavior:
 *   - publisher  "zephyr_heartbeat"  std_msgs/msg/Int32   (1 Hz, green LED blink)
 *   - subscriber "zephyr_led_cmd"    std_msgs/msg/Bool    (toggles blue LED)
 *
 * The application follows the micro-ROS reconnection pattern: it pings the
 * agent, creates the node only while the agent answers, and destroys the node
 * again when the connection is lost. The heartbeat counter advances only after
 * a publish that the agent acknowledged, so the green LED stops pretending to
 * be connected while the Pi is down.
 *
 * LEDs (from the Nucleo F767ZI devicetree aliases):
 *   led0 -> green user LED LD1, PB0 -> slow blink when connected,
 *                                      fast blink when the agent is unreachable
 *   led1 -> blue  user LED LD2, PB7 -> toggled on every received command
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <std_msgs/msg/int32.h>
#include <std_msgs/msg/bool.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <rmw_microros/rmw_microros.h>
#include <rmw_microros/ping.h>

#include "udp_transport.h"

/* One ping is enough to tell whether the agent answers: the timeout covers a
 * busy LAN, and the outer loop decides when to try again.
 */
#define PING_TIMEOUT_MS           100
#define PING_ATTEMPTS             1

/* While connected, ping once a second. The heartbeat timer runs at the same
 * rate, so a dead agent is noticed within roughly one publish period.
 */
#define CONNECTED_PING_PERIOD_MS  1000

/* While waiting for the agent, retry twice a second. The transport opens and
 * closes a socket per ping, so this is also the reconnect latency.
 */
#define RECONNECT_PING_PERIOD_MS  500

/* Fast blink used while the agent is unreachable. It is clearly different from
 * the 1 s heartbeat so the two states cannot be confused.
 */
#define DISCONNECTED_BLINK_MS     100

#define LED_HEARTBEAT_NODE DT_ALIAS(led0)
#define LED_COMMAND_NODE   DT_ALIAS(led1)

static const struct gpio_dt_spec led_heartbeat =
	GPIO_DT_SPEC_GET(LED_HEARTBEAT_NODE, gpios);
static const struct gpio_dt_spec led_command =
	GPIO_DT_SPEC_GET(LED_COMMAND_NODE, gpios);

static bool leds_ready;

static rclc_support_t support;
static rcl_node_t node;
static rclc_executor_t executor;
static rcl_publisher_t heartbeat_publisher;
static std_msgs__msg__Int32 heartbeat_msg;
static rcl_timer_t heartbeat_timer;
static rcl_subscription_t led_cmd_subscription;
static std_msgs__msg__Bool led_cmd_msg;

enum connection_state {
	CONN_WAITING_AGENT,
	CONN_AGENT_AVAILABLE,
	CONN_AGENT_CONNECTED,
	CONN_AGENT_DISCONNECTED,
};

static void heartbeat_led_set(bool on)
{
	if (leds_ready) {
		gpio_pin_set_dt(&led_heartbeat, on ? 1 : 0);
	}
}

static void heartbeat_led_toggle(void)
{
	if (leds_ready) {
		gpio_pin_toggle_dt(&led_heartbeat);
	}
}

static bool period_elapsed(int64_t *last_ms, int64_t period_ms)
{
	int64_t now = k_uptime_get();

	if (now - *last_ms < period_ms) {
		return false;
	}

	*last_ms = now;
	return true;
}

static void update_disconnected_led(int64_t *last_toggle_ms, bool *led_on)
{
	if (!period_elapsed(last_toggle_ms, DISCONNECTED_BLINK_MS)) {
		return;
	}

	*led_on = !*led_on;
	heartbeat_led_set(*led_on);
}

static void heartbeat_timer_callback(rcl_timer_t *timer, int64_t last_call_time)
{
	rcl_ret_t rc;

	RCLC_UNUSED(last_call_time);

	if (timer == NULL) {
		return;
	}

	/* Advance the counter and blink only when the agent confirmed the sample.
	 * The default publisher QoS is reliable, so rcl_publish() waits for the
	 * XRCE acknowledgement and fails when the link is gone. Best-effort
	 * publishing would report success for a UDP datagram that nobody reads.
	 */
	rc = rcl_publish(&heartbeat_publisher, &heartbeat_msg, NULL);
	if (rc != RCL_RET_OK) {
		printk("heartbeat publish failed (%d), not advancing\n", (int)rc);
		return;
	}

	heartbeat_msg.data++;
	heartbeat_led_toggle();
}

static void led_cmd_subscription_callback(const void *msgin)
{
	const std_msgs__msg__Bool *msg = (const std_msgs__msg__Bool *)msgin;
	static int last_state = -1;

	/* Level command, not a toggle: "true" turns the LED on, "false" off.
	 * Idempotent, so publishing the command several times (or retrying
	 * because a DDS discovery race ate the first sample) is harmless.
	 */
	if (leds_ready) {
		gpio_pin_set_dt(&led_command, msg->data ? 1 : 0);
	}

	/* Log only real changes: this is the moment the command took effect, so
	 * the console timestamp shows whether a "slow" command was late on the
	 * wire or late on the board.
	 */
	if ((int)msg->data != last_state) {
		last_state = (int)msg->data;
		printk("led_cmd: %s @ %lld ms\n",
		       msg->data ? "on" : "off", k_uptime_get());
	}
}

static int leds_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led_heartbeat)) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&led_command)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led_heartbeat, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&led_command, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	leds_ready = true;
	return 0;
}

/*
 * Wait until the Ethernet interface has an IPv4 address. Pinging before the
 * address is assigned only produces failed socket opens (and, in the stock
 * module transport, leaked file descriptors).
 */
static void wait_for_ipv4(void)
{
	struct net_if *iface;
	unsigned int elapsed = 0;

	while ((iface = net_if_get_default()) == NULL) {
		k_msleep(100);
	}

	{
		struct net_linkaddr *mac = net_if_get_link_addr(iface);

		if (mac != NULL && mac->len >= 6) {
			printk("net: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			       mac->addr[0], mac->addr[1], mac->addr[2],
			       mac->addr[3], mac->addr[4], mac->addr[5]);
		}
	}

	for (;;) {
		struct in_addr *addr =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_ANY_STATE);

		if (addr != NULL) {
			char buf[NET_IPV4_ADDR_LEN];

			printk("net: %s is %s\n",
			       net_if_get_device(iface)->name,
			       net_addr_ntop(AF_INET, addr, buf, sizeof(buf)));
			return;
		}

		/* Distinguish "no cable" from "the address was never assigned". */
		if (elapsed == 0) {
			printk("net: waiting for an IPv4 address on %s\n",
			       net_if_get_device(iface)->name);
		} else if (elapsed % 2 == 0) {
			printk("net: no IPv4 yet (%us): carrier %s, iface %s\n",
			       elapsed,
			       net_if_is_carrier_ok(iface) ? "up" : "DOWN",
			       net_if_is_up(iface) ? "up" : "DOWN");
		}

		k_msleep(1000);
		elapsed++;
	}
}

/*
 * Create the node and its endpoints. Any partial failure is unwound here so the
 * state machine never has to guess how far creation got.
 */
static bool create_entities(void)
{
	rcl_allocator_t allocator = rcl_get_default_allocator();
	rcl_ret_t rc;

	rc = rclc_support_init(&support, 0, NULL, &allocator);
	if (rc != RCL_RET_OK) {
		printk("micro-ROS init failed (%d)\n", (int)rc);
		return false;
	}

	rc = rclc_node_init_default(&node, "nucleo_f767zi_node", "", &support);
	if (rc != RCL_RET_OK) {
		printk("node creation failed (%d)\n", (int)rc);
		goto fail_support;
	}

	rc = rclc_publisher_init_default(
		&heartbeat_publisher,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32),
		"zephyr_heartbeat");
	if (rc != RCL_RET_OK) {
		printk("heartbeat publisher creation failed (%d)\n", (int)rc);
		goto fail_node;
	}

	rc = rclc_subscription_init_default(
		&led_cmd_subscription,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
		"zephyr_led_cmd");
	if (rc != RCL_RET_OK) {
		printk("LED subscription creation failed (%d)\n", (int)rc);
		goto fail_publisher;
	}

	rc = rclc_timer_init_default2(
		&heartbeat_timer,
		&support,
		RCL_MS_TO_NS(1000),
		heartbeat_timer_callback,
		true);
	if (rc != RCL_RET_OK) {
		printk("heartbeat timer creation failed (%d)\n", (int)rc);
		goto fail_subscription;
	}

	rc = rclc_executor_init(&executor, &support.context, 2, &allocator);
	if (rc != RCL_RET_OK) {
		printk("executor creation failed (%d)\n", (int)rc);
		goto fail_executor;
	}

	rc = rclc_executor_add_timer(&executor, &heartbeat_timer);
	if (rc != RCL_RET_OK) {
		printk("adding the heartbeat timer failed (%d)\n", (int)rc);
		goto fail_executor;
	}

	rc = rclc_executor_add_subscription(
		&executor, &led_cmd_subscription, &led_cmd_msg,
		&led_cmd_subscription_callback, ON_NEW_DATA);
	if (rc != RCL_RET_OK) {
		printk("adding the LED subscription failed (%d)\n", (int)rc);
		goto fail_executor;
	}

	return true;

fail_executor:
	(void)rclc_executor_fini(&executor);
fail_timer:
	(void)rcl_timer_fini(&heartbeat_timer);
fail_subscription:
	(void)rcl_subscription_fini(&led_cmd_subscription, &node);
fail_publisher:
	(void)rcl_publisher_fini(&heartbeat_publisher, &node);
fail_node:
	(void)rcl_node_fini(&node);
fail_support:
	(void)rclc_support_fini(&support);
	return false;
}

static void destroy_entities(void)
{
	rmw_context_t *rmw_context = rcl_context_get_rmw_context(&support.context);

	/* The agent is already gone. Waiting for the session timeout of every
	 * entity would only stall the loop, so destroy them immediately.
	 */
	(void)rmw_uros_set_context_entity_destroy_session_timeout(rmw_context, 0);

	(void)rclc_executor_fini(&executor);
	(void)rcl_timer_fini(&heartbeat_timer);
	(void)rcl_subscription_fini(&led_cmd_subscription, &node);
	(void)rcl_publisher_fini(&heartbeat_publisher, &node);
	(void)rcl_node_fini(&node);
	(void)rclc_support_fini(&support);
}

int main(void)
{
	int ret;
	static app_udp_params_t transport_params = {
		.fd = -1,
	};
	enum connection_state state = CONN_WAITING_AGENT;
	int64_t last_ping_ms;
	int64_t last_blink_ms;
	bool led_on = false;
	unsigned int attempts = 0;

	printk("\nmicro-ROS demo %s\n", APP_VERSION);

	ret = leds_init();
	if (ret < 0) {
		printk("LED init failed (%d), continuing without LEDs\n", ret);
	}

	wait_for_ipv4();

	strcpy(transport_params.ip, CONFIG_MICROROS_AGENT_IP);
	strcpy(transport_params.port, CONFIG_MICROROS_AGENT_PORT);
	printk("micro-ROS UDP agent target %s:%s\n",
	       transport_params.ip, transport_params.port);

	rmw_uros_set_custom_transport(
		false, /* UDP datagrams are already framed by XRCE */
		(void *)&transport_params,
		app_udp_open,
		app_udp_close,
		app_udp_write,
		app_udp_read);

	heartbeat_msg.data = 0;

	/* Ping immediately on the first pass. */
	last_ping_ms = k_uptime_get() - RECONNECT_PING_PERIOD_MS;
	last_blink_ms = k_uptime_get();

	for (;;) {
		switch (state) {
		case CONN_WAITING_AGENT:
			update_disconnected_led(&last_blink_ms, &led_on);

			if (period_elapsed(&last_ping_ms,
					   RECONNECT_PING_PERIOD_MS)) {
				if (rmw_uros_ping_agent(PING_TIMEOUT_MS,
							PING_ATTEMPTS) == RMW_RET_OK) {
					state = CONN_AGENT_AVAILABLE;
					break;
				}

				attempts++;
				if (attempts <= 10 || attempts % 10 == 0) {
					printk("Waiting for the micro-ROS agent (%s:%s), attempt %u...\n",
					       CONFIG_MICROROS_AGENT_IP,
					       CONFIG_MICROROS_AGENT_PORT,
					       attempts);
				}
			}

			k_msleep(10);
			break;

		case CONN_AGENT_AVAILABLE:
			if (!create_entities()) {
				state = CONN_WAITING_AGENT;
				last_ping_ms = k_uptime_get();
				break;
			}

			printk("micro-ROS agent reachable, node created\n");
			printk("micro-ROS node running: publishing /zephyr_heartbeat, subscribing /zephyr_led_cmd\n");
			attempts = 0;
			led_on = false;
			heartbeat_led_set(false);
			last_ping_ms = k_uptime_get();
			state = CONN_AGENT_CONNECTED;
			break;

		case CONN_AGENT_CONNECTED:
			if (period_elapsed(&last_ping_ms,
					   CONNECTED_PING_PERIOD_MS) &&
			    rmw_uros_ping_agent(PING_TIMEOUT_MS,
						PING_ATTEMPTS) != RMW_RET_OK) {
				printk("micro-ROS agent lost, dropping the session\n");
				state = CONN_AGENT_DISCONNECTED;
				break;
			}

			/* Spin with a 1 ms wait instead of 10 ms + k_msleep(10),
			 * so a command is handed to the subscription callback
			 * within ~1-2 ms of arriving on the socket.
			 */
			(void)rclc_executor_spin_some(&executor,
						      RCL_MS_TO_NS(1));
			k_msleep(1);
			break;

		case CONN_AGENT_DISCONNECTED:
			destroy_entities();
			heartbeat_led_set(false);
			led_on = false;
			attempts = 0;
			last_blink_ms = k_uptime_get();
			last_ping_ms = k_uptime_get();
			state = CONN_WAITING_AGENT;
			break;
		}
	}
}
