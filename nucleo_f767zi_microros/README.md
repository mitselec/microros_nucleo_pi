# micro-ROS demo application for NUCLEO-F767ZI on Zephyr

A Zephyr application (this folder is also a **west manifest repository**) that runs a
micro-ROS node over the board's **integrated Ethernet, using UDP transport**.

- Publishes `std_msgs/msg/Int32` on `/zephyr_heartbeat` every 1000 ms (green user LED blinks).
- Subscribes to `std_msgs/msg/Bool` on `/zephyr_led_cmd` (`true` = blue user LED on, `false` = off).
- Waits for the micro-ROS agent at `CONFIG_MICROROS_AGENT_IP:CONFIG_MICROROS_AGENT_PORT`
  and retries until it is reachable.
- Uses a **static IPv4 address** (`CONFIG_NET_CONFIG_MY_IPV4_ADDR`, default
  `192.168.0.230`) so the demo does not depend on the router's DHCP server; DHCP
  can be enabled instead with one line in `prj.conf`.

## Repository layout

```
nucleo_f767zi_microros/
  |- CMakeLists.txt          # Zephyr application build script
  |- prj.conf                # Kconfig (micro-ROS, transport, networking)
  |- west.yml                # west manifest: pulls Zephyr v4.1.0 + micro-ROS module
  |- microros_compat/        # include-path shim module (see note below)
  |- src/main.c              # demo application
  |- src/udp_transport.c     # self-contained UDP transport (see note below)
  |- src/udp_transport.h
```

After `west init`/`west update`, the workspace contains `zephyr/`,
`modules/libmicroros` deps of Zephyr and `modules/micro_ros_zephyr_module/`.

### Why `microros_compat/` exists

The micro-ROS module is registered manually via `ZEPHYR_EXTRA_MODULES`
(its repo is not a west module itself - see `CMakeLists.txt`). Additionally,
its UDP transport code still uses the pre-Zephyr-3.4 header style
(`#include <posix/sys/socket.h>`), unchanged upstream since Dec 2022 in *all*
`micro_ros_zephyr_module` branches (including `rolling`). The
`microros_compat/` shim module restores compatibility on Zephyr >= 3.4 by
adding `include/zephyr` to the include path - no module files are modified.
Consider opening an issue/PR upstream if you read this in a year and the
shim is still needed 🙂.

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
| **v4.1.0** | **Tested green** | `west.yml` pins `revision: v4.1.0`; this is the version verified in CI (`zephyrprojectrtos/zephyr-build:v0.26.17` container) and in the local WSL2 build flow. The `microros_compat/` shim is required for this version. |
| v4.0.0 | Claimed, not tested for UDP | The micro-ROS module README claims v4.0.0 support, but its own CI only builds the **serial** transports. UDP header paths (`posix/sys/socket.h`) need the same shim treatment; untested on this combination. |
| v4.2+ | **Known broken** | Upstream issue [#158](https://github.com/micro-ROS/micro_ros_zephyr_module/issues/158): Zephyr ≥4.2 moved POSIX APIs again (e.g. `zephyr/posix/time.h` → `zephyr/posix/sys/time.h`), breaking compilation for this app. Do not bump `west.yml` past v4.1.0 without re-testing. |
| v3.x | Not supported | The UDP transport header targets Zephyr ≥3.2; pre-3.2 used a different header layout entirely (`zephyr/zephyr.h`, `include/posix/`). |

**Do not edit `west.yml`'s `revision: v4.1.0` unless you're prepared to fix the resulting header/API breakages.** If you bump Zephyr, start from CI's failed-run logs (`Actions` tab on GitHub) and expect to update `microros_compat/CMakeLists.txt` and possibly `modules/libmicroros/microros_transports/udp/*` include paths again.

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

Why static: on a busy home LAN the router can keep offering the address the board
had last time, which another device has taken meanwhile. Zephyr's DHCP client runs
IPv4 address-conflict detection, declines the offer and retries, and on some routers
that loop never ends — the console then shows `net: no IPv4 yet (Ns)` forever while
the link itself is perfectly fine.

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
   A blinking green LED (LD1) means "publishing".

A healthy boot looks like this (the transport opens a socket for the ping and
again for the session, hence the two lines):

```
*** Booting Zephyr OS build v4.1.0 ***
micro-ROS demo v1.0.0
net: MAC 00:80:e1:b4:e0:81
net: ethernet@40028000 is 192.168.0.230
micro-ROS UDP agent target 192.168.0.249:8888
udp: socket 3 connected to 192.168.0.249:8888
micro-ROS agent reachable, creating the node
udp: socket 3 connected to 192.168.0.249:8888
micro-ROS node running: publishing /zephyr_heartbeat, subscribing /zephyr_led_cmd
```

The `micro-ROS demo <version>` line is baked in at build time: CI passes the tag name
(`v1.0.0`) for tagged builds and `main-<sha>` for branch builds, so you can tell which
binary is on the board from the serial log alone. A local build without `-DAPP_VERSION`
prints `dev`.

Every LED command that changes the state is echoed with the board's uptime:

```
led_cmd: on @ 41230 ms
```

That timestamp is when the message was processed, not when you typed the command. If the
line appears immediately but the LED does not, the problem is elsewhere (it cannot be: the
GPIO write happens before the print). If the line appears seconds later, the delay is on
the Pi/network side, not in the firmware — the executor waits only 1 ms
(`rclc_executor_spin_some(1 ms)` + `k_msleep(1)`), so a received sample is handled within
~2 ms.

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
there). The app tolerates a "wrong" agent gracefully: it keeps printing
`Waiting for the micro-ROS agent...` and connects as soon as it answers.

## Test from the Pi

The agent image is minimal and does **not** contain the `ros2` CLI. Use the
multi-arch `ros:humble` image on the host network:

```bash
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic list"
# should show /zephyr_heartbeat and /zephyr_led_cmd

# blue LED on (true) / off (false). Publishing a few times is harmless and
# avoids losing the command to a DDS discovery race:
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: true}'"
```

Every `docker run` starts a **new DDS participant**, and the micro-ROS agent can take a few
seconds to match it. A single `ros2 topic pub --once` may therefore be published into the void
and exit before the match. That is why the command is a level (`true` = on) instead of a
toggle: sending it repeatedly always converges to the same state.

## Serial transport fallback (optional)

If you cannot use Ethernet, switch the transport in `prj.conf`:

```ini
CONFIG_MICROROS_TRANSPORT_SERIAL=y      # instead of UDP, also remove CONFIG_MICROROS_TRANSPORT_UDP=y
CONFIG_MICROROS_SERIAL_PORT="1"        # see module Kconfig; "2" = USART6 on the ST Zio/Arduino header
```

and run the agent with `serial --dev <tty>` instead of `udp4`. Note: with serial, `micro-ROS`
framing is required (the Zephyr module's serial transports handle this via
`MICRO_ROS_FRAMING_REQUIRED`).

## Troubleshooting (symptoms seen in practice)

| Console / symptom | Cause | Fix |
|---|---|---|
| `net: no IPv4 yet (Ns): carrier up, iface up` repeating forever | Link is fine, no usable DHCP lease (often an address-conflict loop on the router) | Use the static-IP block in `prj.conf`, or give the router a reservation for the board's MAC |
| `net: no IPv4 yet (...): carrier DOWN` | No link: cable, switch port, or PHY | Try another cable/port; check the RJ45 LEDs |
| `Waiting for the micro-ROS agent (IP:PORT), attempt N...` forever | Agent not running, wrong IP/port baked into the firmware, a firewall, or a session the agent still holds for the previous boot | `docker ps`/`docker logs microros-agent` on the Pi; check that `CONFIG_MICROROS_AGENT_IP` matches `hostname -I`. Then press the board's **RESET** (the client connects at boot). If it still retries, `docker restart microros-agent` and reset again |
| `udp: connect() to ... failed (errno N)` | No route yet (interface has no address) | The app waits for the address first; if it persists, check the subnet/netmask |
| PC cannot ping the board right after flashing (`Destination host unreachable`) | ARP not resolved yet while the board is still booting | Wait a few seconds and retry; `arp -a` should then show the board's MAC |

The board prints its MAC at boot (`net: MAC ...`), which is handy when checking the
router's client list.

### "I had to reset the board before it connected"

The micro-ROS client creates its session at boot, so the usual order is: **start the
agent, then reset the board**. In our tests a board that booted while the agent was
already up sometimes kept retrying even though the agent answered XRCE pings from
other hosts; restarting the agent and resetting the board always cleared it. The
retry loop never gives up, so nothing has to be reflashed once the agent answers.

## Known limitations

- The micro-ROS library is built as an ExternalProject on first build; a `make`/`colcon`
  sub-build failure most often means missing Python deps (step 1) or a Windows host build.
- Older F767ZI boards have shown flaky Ethernet in some Zephyr versions
  ([zephyr#26519](https://github.com/zephyrproject-rtos/zephyr/issues/26519),
  closed). If ping works but other traffic drops, try a different cable/switch
  port and check the board revision before blaming the firmware.
- This module's Kconfig resource counters (`CONFIG_MICROROS_PUBLISHERS` etc.) are `string`
  typed oddities; leave the defaults unless you add more endpoints than the demo uses.
