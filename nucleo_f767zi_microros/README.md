# micro-ROS demo application for NUCLEO-F767ZI on Zephyr

A Zephyr application (this folder is also a **west manifest repository**) that runs a
micro-ROS node over the board's **integrated Ethernet, using UDP transport**.

- Publishes `std_msgs/msg/Int32` on `/zephyr_heartbeat` every 1000 ms while the
  agent is reachable (green user LED blinks).
- Subscribes to `std_msgs/msg/Bool` on `/zephyr_led_cmd` (`true` = blue user LED on, `false` = off).
- Pings the micro-ROS agent at `CONFIG_MICROROS_AGENT_IP:CONFIG_MICROROS_AGENT_PORT`,
  creates the node only while the agent answers, and destroys it again when the
  connection is lost. The heartbeat counter and the green LED stop when the
  agent is unreachable, and the board reconnects on its own when it returns.
- Uses a **static IPv4 address** (`CONFIG_NET_CONFIG_MY_IPV4_ADDR`, default
  `192.168.0.230`) so the demo does not depend on the router's DHCP server; DHCP
  can be enabled instead with one line in `prj.conf`.

## Repository layout

```
nucleo_f767zi_microros/
  |- CMakeLists.txt          # Zephyr application build script
  |- prj.conf                # Kconfig (micro-ROS, transport, networking)
  |- prj_picolibc.conf       # picolibc overlay for Zephyr 4.4+ (see note below)
  |- west.yml                # west manifest: pulls Zephyr v4.1.0 + micro-ROS module
  |- microros_compat/        # compatibility module (see note below)
  |- src/main.c              # demo application
  |- src/udp_transport.c     # self-contained UDP transport (see note below)
  |- src/udp_transport.h
```

After `west init`/`west update`, the workspace contains `zephyr/`,
`modules/libmicroros` deps of Zephyr and `modules/micro_ros_zephyr_module/`.

### Why `microros_compat/` exists

The micro-ROS module is registered manually via `ZEPHYR_EXTRA_MODULES`
(its repo is not a west module itself - see `CMakeLists.txt`). The
`microros_compat/` module then carries the small fixes needed to build the
module against newer Zephyr releases, without modifying any module file:

- It adds `include/zephyr` to the include path, because the module's UDP
  transport still uses the pre-Zephyr-3.4 header style
  (`#include <posix/sys/socket.h>`). Upstream has not changed this since
  Dec 2022 in *any* `micro_ros_zephyr_module` branch, including `rolling`.
- It forwards `zephyr/posix/time.h` to `<time.h>` on Zephyr 4.3+, where the
  old header was removed. Zephyr defines `_POSIX_C_SOURCE=200809L` for the
  build, so the C library header already declares the POSIX time APIs and
  including `posix_time.h` as well would duplicate them.
- It declares `strcasecmp()`, `strncasecmp()` and `isatty()` for picolibc,
  where Zephyr's POSIX headers do not expose them, and provides a weak
  `isatty()` stub. rcutils only uses it to decide whether to colourise log
  output.
- It disables rcutils' process support (`RCUTILS_NO_PROCESS_SUPPORT`),
  because Zephyr implements neither `fork()` nor `execvp()`, and turns off
  rcutils' Annex K request for picolibc.
- It adds the generated `zephyr/include/generated/zephyr` directory to the
  include path, where Zephyr 4.5 keeps `<version.h>`.

### Why the app ships its own UDP transport

The module's `microros_transports/udp` implementation is not exercised by
upstream CI (which builds the serial transports only) and has two problems that
show up on a real network:

1. It resolves the agent address with `getaddrinfo()`, which allocates memory
   and is unnecessary for a numeric IPv4 address.
2. If `socket()` succeeds but `connect()` fails (normal during boot, before the
   interface has an address), it returns `false` **without closing the socket**.
   `rmw_uros_ping_agent()` retries in a loop, so the leaked descriptors
   accumulate until `socket()` fails permanently: the board then never sends a
   single UDP packet, even though ICMP ping still works.

`src/udp_transport.c` replaces it with a small transport that parses the
address with `zsock_inet_pton()`, closes the socket on every failure path, and
prints the reason (`udp: connect() ... failed (errno N)`). `main.c` also waits
for an IPv4 address before the first ping. The module's transport library is
still built because the Kconfig choice selects it, but the app no longer uses it.

## Zephyr version compatibility

| Zephyr version | Status | Notes |
|---|---|---|
| **v4.1.0** | **Runtime tested** | `west.yml` pins `revision: v4.1.0`. Built in CI and run on the board. The `microros_compat/` module is required. |
| v4.0.0 | Claimed, not tested for UDP | The module README claims support, but its CI only builds the **serial** transports. UDP needs the same compatibility module; untested here. |
| v4.2.0 | Builds in CI | Newlib and picolibc. Not run on hardware. |
| v4.3.0 | Builds in CI with newlib | Zephyr's own POSIX timer implementation prevents the picolibc build. Not run on hardware. |
| v4.4.0 and v4.5.0 | Builds in CI with picolibc | Requires Zephyr SDK 1.0.x and the `prj_picolibc.conf` overlay. Not run on hardware. |
| v3.x | Not supported | The transport targets Zephyr ≥3.2; earlier versions used a different header layout. |

`west.yml` remains pinned at v4.1.0 for the normal build. Newer releases are
build-verified only; run the "Zephyr compatibility probe" workflow manually from
the Actions tab to check them without changing the production manifest.

## Configuring the agent address

Edit `prj.conf` before building:

```ini
CONFIG_MICROROS_AGENT_IP="192.168.1.100"   # <- your Raspberry Pi IP
CONFIG_MICROROS_AGENT_PORT="8888"          # <- the UDP port the agent listens on
```

The board uses a **static address** by default (`192.168.0.230`, see the Networking
section of `prj.conf`). Pick an address that is free on your LAN and outside your
router's DHCP pool, and make sure it is in the same subnet as the Pi. To use DHCP
instead, comment out the `CONFIG_NET_CONFIG_*` lines and uncomment `CONFIG_NET_DHCPV4=y`.

The static address avoids a DHCP failure mode: on a busy LAN the router can offer an
address that another device has taken, Zephyr's IPv4 address-conflict detection declines
it, and on some routers the retry loop never ends (`net: no IPv4 yet (Ns)` with the link
up).

## Building (WSL2 on Windows is recommended)

The micro-ROS client library is built with `make`, `colcon` and Python - run the build
inside **WSL2** (the `micro_ros_zephyr_module` README itself used a Linux docker image).

### 1. WSL + toolchain, one-time setup

Windows side (PowerShell):

```powershell
wsl --install -d Ubuntu-24.04
```

Inside WSL:

```bash
sudo apt update && sudo apt install -y build-essential python3-pip python3-venv git wget xxd
pip3 install --break-system-packages catkin_pkg lark-parser empy colcon-common-extensions west
```

Zephyr SDK (provides the `arm-zephyr-eabi` toolchain):

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.9/zephyr-sdk-0.16.9_linux-x86_64.tar.xz
tar xzf zephyr-sdk-0.16.9_linux-x86_64.tar.xz
cd zephyr-sdk-0.16.9 && ./zephyr-sdk-0.16.9-setup.sh -c
```

Zephyr 4.1 to 4.3 use SDK 0.16.x. Zephyr 4.4 and newer require SDK 1.0.x,
which no longer ships newlib; install the matching release from the same
download page and point `ZEPHYR_SDK_INSTALL_DIR` at it.

### 2. Initialize the workspace and build

```bash
cd /mnt/d/Programming/ROS2/nucleo_f767zi_microros
west init -l .       # creates .west workspace in this folder (source stays where it is)
west update          # clones zephyr + all deps + micro_ros_zephyr_module
pip3 install -r zephyr/scripts/requirements.txt

export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.9

west build -b nucleo_f767zi -p always .
```

If the micro-ROS module is not auto-discovered on your west version, add it explicitly:

```bash
west build -b nucleo_f767zi -p always . -- \
  -DZEPHYR_EXTRA_MODULES=$PWD/../modules/micro_ros_zephyr_module/modules/libmicroros
```

#### Zephyr 4.4 and newer (picolibc)

`west.yml` pins v4.1.0, so a normal build uses newlib. To build a newer
release, change the manifest revision and add the picolibc overlay:

```bash
sed -i 's/revision: v4.1.0/revision: v4.5.0/' west.yml
west update
west build -b nucleo_f767zi -p always . -- \
  -DEXTRA_CONF_FILE=prj_picolibc.conf
```

Zephyr 4.3 is the exception: its POSIX timer code does not compile against
picolibc, so use newlib there. Builds for 4.4 and 4.5 have been verified in CI
but not run on hardware; keep `west.yml` at v4.1.0 for the normal demo.

### 3. Flashing

`west flash` requires STM32CubeProgrammer in PATH (default runner for this board). The
easiest mix on Windows: build in WSL, flash from Windows with STM32CubeProgrammer CLI:

```powershell
STM32_Programmer_CLI -c port=SWD mode=UR reset=HWrst -w D:\Programming\ROS2\nucleo_f767zi_microros\build\zephyr\zephyr.hex -v -g
```

Use `mode=UR reset=HWrst` (connect under reset). Plain `mode=NORMAL` fails on some
Nucleo boards with `Error: Unable to read device id from ROM table`
(`ST-LINK error (DEV_CONNECT_ERR)`), because the running firmware keeps the debug
port busy.

(or install STM32CubeProgrammer inside WSL2 and just run `west flash`; OpenOCD/J-Link
runners also exist on this board).

### 4. Run

1. Connect the RJ45 to the same switch/router as the Pi.
2. Start the agent on the Pi (see `../raspberry_pi_agent/README.md`).
3. Reset the board. The console (ST-LINK VCP, USART3 @115200) prints agent/address info.
   A green LED (LD1) that toggles once per second means "connected and publishing".
   A fast blink (100 ms) means "running, but the agent is not answering".

A healthy boot looks like this (the first successful socket open is logged; later
reconnects reuse the same transport without repeating the line):

```
*** Booting Zephyr OS build v4.1.0 ***
micro-ROS demo v1.0.0
net: MAC 00:80:e1:b4:e0:81
net: ethernet@40028000 is 192.168.0.230
micro-ROS UDP agent target 192.168.0.249:8888
udp: socket 3 connected to 192.168.0.249:8888
micro-ROS agent reachable, node created
micro-ROS node running: publishing /zephyr_heartbeat, subscribing /zephyr_led_cmd
```

The `micro-ROS demo <version>` line is baked in at build time: CI passes the tag name
(`v1.0.0`) for tagged builds and `main-<sha>` for branch builds, so the serial log
identifies the binary. A local build without `-DAPP_VERSION` prints `dev`.

Every LED command that changes the state is echoed with the board's uptime:

```
led_cmd: on @ 41230 ms
```

The timestamp is when the callback ran. The GPIO write happens before the print, so a late
line means the sample arrived late, not that the board was slow. The main loop waits 1 ms
(`rclc_executor_spin_some(1 ms)` + `k_msleep(1)`), so a received sample is handled within
~2 ms.

### Agent loss and reconnect

The heartbeat counter advances only after `rcl_publish()` confirms that the agent
received the sample. The default publisher QoS is reliable, so `rcl_publish()`
waits for the agent to acknowledge the sample and returns an error when that
acknowledgement does not arrive within the session timeout. The timer callback
does not increment the counter or toggle the LED after a failed publish.

A ping once per second is the second line of defence. When it fails, the node and
its endpoints are destroyed, the green LED switches to the fast blink, and the
board keeps pinging every 500 ms. When the agent answers again, the node is
created from scratch. The counter keeps its value across reconnects, so it still
counts heartbeats since boot.

A disconnect looks like this on the console:

```
micro-ROS agent lost, dropping the session
Waiting for the micro-ROS agent (192.168.0.249:8888), attempt 1...
Waiting for the micro-ROS agent (192.168.0.249:8888), attempt 2...
micro-ROS agent reachable, node created
micro-ROS node running: publishing /zephyr_heartbeat, subscribing /zephyr_led_cmd
```

The wait line is printed for the first ten attempts and then every tenth attempt,
so a long outage does not flood the console.

## Downloading & flashing CI firmware (no local toolchain needed)

Every run of the [CI workflow](https://github.com/mitselec/microros_nucleo_pi/actions/workflows/ci.yml)
builds the firmware and attaches it as an artifact:

1. Open the **Actions** tab, pick the latest green run on `main`.
2. Scroll to **Artifacts** and download `nucleo_f767zi-microros-firmware`, unzip it.
3. Flash over the board's ST-LINK USB with STM32CubeProgrammer:
   ```powershell
   STM32_Programmer_CLI -c port=SWD mode=UR reset=HWrst -w zephyr.hex -v -g
   ```
   or via the GUI: Connect (SWD) -> Open File -> `zephyr.hex` -> Download.

**Caveat:** `CONFIG_MICROROS_AGENT_IP` in `nucleo_f767zi_microros/prj.conf` is baked into
the firmware at build time. CI builds use whatever that file contains in the repo - so if
you flash repo artifacts, make sure your Pi's IP matches it (or commit your known IP
there). The app keeps retrying, so it connects as soon as the address answers.

## Test from the Pi

The agent image is minimal and does **not** contain the `ros2` CLI. Use the
multi-arch `ros:humble` image on the host network:

```bash
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic list"
# should show /zephyr_heartbeat and /zephyr_led_cmd

# blue LED on (true) / off (false). Repeated publishes are harmless:
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: true}'"
```

A new container is a new DDS participant and the agent needs a moment to match it, so a
single `--once` publish can be lost. The command is a level (`true` = on), so repeating it
is harmless.

## Serial transport fallback (optional)

If you cannot use Ethernet, switch the transport in `prj.conf`:

```ini
CONFIG_MICROROS_TRANSPORT_SERIAL=y      # instead of UDP, also remove CONFIG_MICROROS_TRANSPORT_UDP=y
CONFIG_MICROROS_SERIAL_PORT="1"        # see module Kconfig; "2" = USART6 on the ST Zio/Arduino header
```

and run the agent with `serial --dev <tty>` instead of `udp4`. Serial needs XRCE
framing; the module's serial transports handle it via `MICRO_ROS_FRAMING_REQUIRED`.

## Troubleshooting

| Console / symptom | Cause | Fix |
|---|---|---|
| `net: no IPv4 yet (Ns): carrier up, iface up` repeating forever | Link is fine, no usable DHCP lease (often an address-conflict loop on the router) | Use the static-IP block in `prj.conf`, or give the router a reservation for the board's MAC |
| `net: no IPv4 yet (...): carrier DOWN` | No link: cable, switch port, or PHY | Try another cable/port; check the RJ45 LEDs |
| Green LED blinks fast (100 ms) | Board is running, but the agent is not answering | Start the agent; check that `CONFIG_MICROROS_AGENT_IP` matches `hostname -I` and that port 8888 is reachable |
| `Waiting for the micro-ROS agent (IP:PORT), attempt N...` repeating | Agent not running, wrong IP/port baked into the firmware, a firewall, or a session the agent still holds for the previous boot | `docker ps`/`docker logs microros-agent` on the Pi; check that `CONFIG_MICROROS_AGENT_IP` matches `hostname -I`. The board retries on its own; `docker restart microros-agent` if the old session does not expire |
| `heartbeat publish failed (N), not advancing` | The agent stopped acknowledging while the ping still succeeded | Expected during a disconnect; the next ping drops the session and the board reconnects |
| `udp: connect() to ... failed (errno N)` | No route yet (interface has no address) | The app waits for the address first; if it persists, check the subnet/netmask |
| PC cannot ping the board right after flashing (`Destination host unreachable`) | ARP not resolved yet while the board is still booting | Wait a few seconds and retry; `arp -a` should then show the board's MAC |

The board prints its MAC at boot (`net: MAC ...`), which is handy when checking the
router's client list.

### Connection order

The board can boot before or after the agent. It pings until the agent answers, creates the
session, and recreates it if the agent restarts. If the agent still holds the session from
the previous boot, session creation can fail until that entry expires; the board keeps
retrying, so no reset or reflash is needed. `docker restart microros-agent` clears the old
session immediately.

## Known limitations

- The micro-ROS library is built as an ExternalProject on first build; a `make`/`colcon`
  sub-build failure most often means missing Python deps (step 1) or a Windows host build.
- Older F767ZI boards have shown flaky Ethernet in some Zephyr versions
  ([zephyr#26519](https://github.com/zephyrproject-rtos/zephyr/issues/26519),
  closed). If ping works but other traffic drops, try a different cable/switch
  port and check the board revision before blaming the firmware.
- This module's Kconfig resource counters (`CONFIG_MICROROS_PUBLISHERS` etc.) are `string`
  typed oddities; leave the defaults unless you add more endpoints than the demo uses.
- The agent-loss and reconnect path follows the micro-ROS state machine and builds in CI,
  but the outage and restart sequence has not been exercised on hardware here. If the board
  does not come back after the agent restarts, capture the console log and the agent log.
