# Changelog

Each section becomes the body of the matching GitHub release. The `release` job in
`.github/workflows/ci.yml` extracts the section whose heading matches the pushed tag
(`v1.0.1` -> `## [1.0.1]`) and appends the flashing instructions. Add the section
before pushing the tag; without one, the release falls back to a generic description.

## [Unreleased]

The heartbeat now reflects the real link state.

### Changed

- The node follows the micro-ROS reconnection state machine. It pings the agent
  once per second while connected and every 500 ms while reconnecting, destroys
  the node when the ping fails, and creates it again when the agent returns.
- The heartbeat counter and the green LED advance only after `rcl_publish()`
  confirms the sample. A failed publish no longer increments the counter or
  toggles the LED.
- The green LED blinks fast (100 ms) while the agent is unreachable, instead of
  continuing the 1 Hz heartbeat.
- The transport logs the first successful socket open only, so reconnect attempts
  do not flood the console.

## [1.0.1] - 2026-11-23

Faster LED commands, and a version stamp in the boot log.

### Added

- Boot banner: `micro-ROS demo v1.0.1`. CI passes the tag name (or `main-<sha>`) as
  `-DAPP_VERSION`; local builds print `dev`.
- Console line `led_cmd: on @ 41230 ms` when a command changes the LED, with the
  board's uptime.
- `led-ctl` compose service: one long-lived publisher that reads `ctrl/led`, so a
  command does not pay for container start, Python start and DDS discovery.

### Changed

- Executor poll reduced from 10 ms + `k_msleep(10)` to 1 ms + `k_msleep(1)`.
- Agent verbosity `-v6` -> `-v4`. `-v6` writes every XRCE packet to Docker's log
  file, which is slow on an SD card.
- Pi guide: latency breakdown, measurement commands, and the DDS implementation
  options.

## [1.0.0] - 2026-11-23

First working end-to-end demo: NUCLEO-F767ZI <-> Raspberry Pi 3B+.

### Added

- micro-ROS node on Zephyr 4.1.0: `/zephyr_heartbeat` (`std_msgs/Int32`, 1 Hz) and
  `/zephyr_led_cmd` (`std_msgs/Bool`) over UDP/IPv4 Ethernet, port 8888.
- Local UDP transport (`src/udp_transport.c`): one socket per session, closed on all
  failure paths, with diagnostics for `socket()`, `connect()`, `send()` and `poll()`.
- Static IPv4 address `192.168.0.230` (DHCP documented as an alternative), MAC and
  carrier diagnostics at boot.
- LED command is a level (`true` = on, `false` = off), so repeats are idempotent.
- Docker agent on the Pi (`microros/micro-ROS-agent:kilted`, host network,
  `udp4 --port 8888`) and `ros:humble` CLI instructions.
- GitHub Actions: build on firmware changes (docs-only commits are skipped),
  firmware artifacts, and a release for every `v*` tag.
