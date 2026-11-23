# Changelog

Each version section below becomes the body of the matching GitHub release: the
`release` job in `.github/workflows/ci.yml` extracts the section whose heading
matches the pushed tag (`v1.0.1` -> `## [1.0.1]`) and appends the flashing
instructions. Add the section **before** pushing the tag; if there is no
matching section the release falls back to a generic description.

## [1.0.1] - 2026-11-23

Faster LED commands, and firmware you can identify from the console.

### Added

- **Boot banner with the build version**: `micro-ROS demo v1.0.1`. CI bakes in
  the tag name, or `main-<sha>` for branch builds, so the serial log alone tells
  you which binary is on the board. Local builds print `dev`.
- **Command trace**: the console prints `led_cmd: on @ 41230 ms` when a command
  changes the LED, with the board's uptime. A command that "does nothing" is now
  either visibly late on the wire or visibly handled on the board.
- **`led-ctl` service** (`raspberry_pi_agent/`): one long-lived ROS 2 publisher
  that watches a file. `echo true > ctrl/led` switches the LED on in tens of
  milliseconds, instead of the seconds a fresh `docker run ros:humble ros2 topic
  pub` spends on container start, Python start and DDS discovery.

### Changed

- The executor polls every 1 ms instead of 10 ms + `k_msleep(10)`, removing up
  to ~20 ms between a sample arriving and the callback running.
- Agent verbosity lowered from `-v6` to `-v4`: `-v6` writes every XRCE packet to
  Docker's log file, which costs real time on a Pi's SD card.
- The Pi guide now explains where the latency comes from, how to measure it, and
  why Cyclone DDS cannot replace Fast DDS in the agent (the agent only has
  `fastdds` and `ced` backends) - it can only be used on the CLI/`led-ctl` side.

## [1.0.0] - 2026-11-23

First working end-to-end demo: NUCLEO-F767ZI <-> Raspberry Pi 3B+.

### Added

- micro-ROS node on Zephyr 4.1.0 publishing `/zephyr_heartbeat`
  (`std_msgs/Int32`, 1 Hz) and subscribing `/zephyr_led_cmd` (`std_msgs/Bool`)
  over **UDP/IPv4 Ethernet**, port 8888.
- Local UDP transport for micro-ROS on Zephyr (`src/udp_transport.c`): one
  socket per session, always closed on the failure paths, with diagnostics for
  `socket()`, `connect()`, `send()` and `poll()` errors.
- Static IPv4 address `192.168.0.230` (DHCP kept as a documented alternative)
  plus MAC and carrier diagnostics at boot.
- The blue LED follows the command as a **level** (`true` = on, `false` = off),
  so repeating a command is idempotent.
- Docker micro-ROS agent on the Pi (`microros/micro-ROS-agent:kilted`, host
  network, `udp4 --port 8888`) and `ros:humble` CLI instructions.
- GitHub Actions: build on firmware changes (docs-only commits are skipped),
  firmware artifacts on every run, and a GitHub release for every `v*` tag.
