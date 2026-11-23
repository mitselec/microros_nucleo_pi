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
 * LEDs (from the Nucleo F767ZI devicetree aliases):
 *   led0 -> green user LED LD1, PB0 -> toggled on every publish
 *   led1 -> blue  user LED LD2, PB7 -> toggled on every received command
 */

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

#define RCCHECK(fn)                                                            \
	{                                                                          \
		rcl_ret_t temp_rc = fn;                                                \
		if ((temp_rc != RCL_RET_OK)) {                                         \
			printk("micro-ROS failure at line %d: %d. Aborting.\n",            \
			       __LINE__, (int)temp_rc);                                    \
			for (;;) {                                                         \
				;                                                              \
			}                                                                  \
		}                                                                      \
	}

#define RCSOFTCHECK(fn)                                                        \
	{                                                                          \
		rcl_ret_t temp_rc = fn;                                                \
		if ((temp_rc != RCL_RET_OK)) {                                         \
			printk("micro-ROS soft failure at line %d: %d. Continuing.\n",     \
			       __LINE__, (int)temp_rc);                                    \
		}                                                                      \
	}

#define LED_HEARTBEAT_NODE DT_ALIAS(led0)
#define LED_COMMAND_NODE   DT_ALIAS(led1)

static const struct gpio_dt_spec led_heartbeat =
	GPIO_DT_SPEC_GET(LED_HEARTBEAT_NODE, gpios);
static const struct gpio_dt_spec led_command =
	GPIO_DT_SPEC_GET(LED_COMMAND_NODE, gpios);

static rcl_publisher_t heartbeat_publisher;
static std_msgs__msg__Int32 heartbeat_msg;
static rcl_timer_t heartbeat_timer;
static rcl_subscription_t led_cmd_subscription;
static std_msgs__msg__Bool led_cmd_msg;

static void heartbeat_timer_callback(rcl_timer_t *timer, int64_t last_call_time)
{
	RCLC_UNUSED(last_call_time);

	if (timer != NULL) {
		RCSOFTCHECK(rcl_publish(&heartbeat_publisher, &heartbeat_msg, NULL));
		heartbeat_msg.data++;
		gpio_pin_toggle_dt(&led_heartbeat);
	}
}

static void led_cmd_subscription_callback(const void *msgin)
{
	const std_msgs__msg__Bool *msg = (const std_msgs__msg__Bool *)msgin;

	/* Level command, not a toggle: "true" turns the LED on, "false" off.
	 * Idempotent, so publishing the command several times (or retrying
	 * because a DDS discovery race ate the first sample) is harmless.
	 */
	gpio_pin_set_dt(&led_command, msg->data ? 1 : 0);
}

static int leds_init(void)
{
	int ret = 0;

	if (!gpio_is_ready_dt(&led_heartbeat)) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&led_command)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led_heartbeat, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&led_command, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

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

int main(void)
{
	int ret;

	ret = leds_init();
	if (ret < 0) {
		printk("LED init failed (%d), continuing without LEDs\n", ret);
	}

	wait_for_ipv4();

	static app_udp_params_t transport_params = {
		.fd = -1,
	};

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

	/* Wait for the agent to answer. Log every attempt while the link is
	 * still settling, then back off to every fifth attempt.
	 */
	int attempts = 0;
	while (rmw_uros_ping_agent(1000, 1) != RMW_RET_OK) {
		attempts++;
		if (attempts <= 10 || attempts % 5 == 0) {
			printk("Waiting for the micro-ROS agent (%s:%s), attempt %d...\n",
			       CONFIG_MICROROS_AGENT_IP,
			       CONFIG_MICROROS_AGENT_PORT,
			       attempts);
		}
		k_msleep(1000);
	}
	printk("micro-ROS agent reachable, creating the node\n");

	rcl_allocator_t allocator = rcl_get_default_allocator();
	rclc_support_t support;

	/* create init_options */
	RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));

	/* create node */
	rcl_node_t node;
	RCCHECK(rclc_node_init_default(&node, "nucleo_f767zi_node", "", &support));

	/* create publisher (1 Hz heartbeat counter) */
	RCCHECK(rclc_publisher_init_default(
		&heartbeat_publisher,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32),
		"zephyr_heartbeat"));

	/* create subscription to the led_cmd topic */
	RCCHECK(rclc_subscription_init_default(
		&led_cmd_subscription,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
		"zephyr_led_cmd"));

	/* create periodic timer for the heartbeat */
	const unsigned int timer_timeout = 1000;
	RCCHECK(rclc_timer_init_default2(
		&heartbeat_timer,
		&support,
		RCL_MS_TO_NS(timer_timeout),
		heartbeat_timer_callback,
		true));

	/* create executor: timer + subscription */
	rclc_executor_t executor;
	RCCHECK(rclc_executor_init(&executor, &support.context, 2, &allocator));
	RCCHECK(rclc_executor_add_timer(&executor, &heartbeat_timer));
	RCCHECK(rclc_executor_add_subscription(
		&executor, &led_cmd_subscription, &led_cmd_msg,
		&led_cmd_subscription_callback, ON_NEW_DATA));

	heartbeat_msg.data = 0;

	printk("micro-ROS node running: publishing /zephyr_heartbeat, subscribing /zephyr_led_cmd\n");
	while (1) {
		rclc_executor_spin_some(&executor, RCL_MS_TO_NS(10));
		k_msleep(10);
	}

	/* free unused resources (not reached in this demo) */
	RCCHECK(rcl_publisher_fini(&heartbeat_publisher, &node));
	RCCHECK(rcl_subscription_fini(&led_cmd_subscription, &node));
	RCCHECK(rcl_timer_fini(&heartbeat_timer));
	RCSOFTCHECK(rclc_executor_fini(&executor));
	RCCHECK(rcl_node_fini(&node));
	return 0;
}
