#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Low-latency control of /zephyr_led_cmd.

Why this exists
---------------
`docker run ros:humble ... ros2 topic pub --once ...` costs several seconds
per command, and none of it is the board:

  * starting a container                          ~0.5-1 s
  * starting Python + the `ros2` CLI on a Pi 3B+  ~2-4 s
  * a fresh DDS participant the agent must find   ~0.2-1 s
  * `--once`/`--times` also wait for that match

This process starts once and keeps a publisher that is already matched with
the agent. It watches a control file and publishes only when the value
changes, so a command reaches the board in tens of milliseconds.

Usage on the Pi (inside the `led-ctl` container from docker-compose.yml):

    echo true  > raspberry_pi_agent/ctrl/led   # blue LED on
    echo false > raspberry_pi_agent/ctrl/led   # blue LED off
"""

import os
import time

import rclpy
from rclpy.node import Node
from std_msgs.msg import Bool

CTRL_FILE = os.environ.get("LED_CTRL_FILE", "/ctrl/led")
POLL_INTERVAL = 0.02      # 50 Hz: how often the control file is read
REPUBLISH_EVERY = 2.0     # re-assert the state (the command is a level, so
                          # this is harmless and recovers a board that rebooted)

TRUE_WORDS = {"1", "true", "on", "yes"}
FALSE_WORDS = {"0", "false", "off", "no"}


def read_state():
    """Return True/False from the control file, or None if unreadable/empty."""
    try:
        with open(CTRL_FILE, "r", encoding="utf-8") as handle:
            word = handle.read().strip().lower()
    except OSError:
        return None

    if word in TRUE_WORDS:
        return True
    if word in FALSE_WORDS:
        return False
    return None


def main():
    rclpy.init()
    node = Node("led_ctl")
    publisher = node.create_publisher(Bool, "/zephyr_led_cmd", 10)
    node.get_logger().info(
        "watching %s - write 'true' or 'false' into it" % CTRL_FILE)

    last_state = None
    last_sent = 0.0

    try:
        while rclpy.ok():
            state = read_state()
            now = time.monotonic()

            if state is not None:
                changed = state != last_state
                due = (now - last_sent) >= REPUBLISH_EVERY

                if changed or due:
                    publisher.publish(Bool(data=state))
                    last_sent = now
                    if changed:
                        last_state = state
                        node.get_logger().info("published %s" % state)

            time.sleep(POLL_INTERVAL)
    except KeyboardInterrupt:
        pass

    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
