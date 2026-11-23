# micro-ROS agent + ROS 2 on Raspberry Pi 3B+

The Pi hosts the **micro-ROS agent**: the bridge that speaks the micro-ROS (XRCE-DDS)
protocol with the Nucleo (UDP over Ethernet, port 8888) and switches to plain ROS 2
DDS on the other side, so the Nucleo appears as an ordinary node in the ROS 2 graph.

```
ros2 CLI (Docker, host net)  <--DDS-->  micro-ROS agent (Docker, UDP:8888)  <--UDP-->  Nucleo F767ZI
```

## 1. Prepare the Pi (one-time)

1. Flash a **64-bit** OS image (Raspberry Pi OS 64-bit or Ubuntu Server 24.04) - 32-bit
   images are not supported by the arm64 agent image.
2. Connect the Pi's Ethernet to the same switch/LAN as the Nucleo.
3. Install Docker:

   ```bash
   curl -fsSL https://get.docker.com | sudo sh
   sudo usermod -aG docker $USER
   # re-login afterwards
   ```

4. Note the Pi's IP address - the Nucleo firmware must point at it:

   ```bash
   hostname -I
   ```

   Put that value into the Nucleo project's `prj.conf`
   (`CONFIG_MICROROS_AGENT_IP="..."`).

## 2. Start the agent

Either with compose (recommended, survives reboots):

```bash
cd raspberry_pi_agent   # copy this folder onto the Pi
docker compose up -d
```

or with the helper script (`chmod +x run-agent.sh` first, then `./run-agent.sh`).

Equivalent one-liner (no compose file needed):

```bash
docker run -d --name microros-agent --restart unless-stopped --net=host --privileged \
  -v /dev:/dev microros/micro-ros-agent:kilted udp4 --port 8888 -v6
```

The `microros/micro-ros-agent:kilted` image is published for `linux/amd64` and
`linux/arm64`, so it runs directly on the Pi 3B+. Match the tag to the firmware's
`micro_ros_zephyr_module` branch (both are `kilted` here).

## 3. Check it

```bash
docker logs -f microros-agent
```

With the agent alone it says it is listening (`CLI ... port 8888`). Once the Nucleo
connects, you will see session/client activity lines.

## 4. Use it: ros2 CLI in a container

The agent image is minimal and does **not** contain the `ros2` CLI. Use the
multi-arch `ros:humble` image on the host network instead:

```bash
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic list"
# expect:
#   /zephyr_heartbeat
#   /zephyr_led_cmd
#   /parameters
```

Switch the blue LED on and off while watching the board:

```bash
# true = on, false = off (a short burst is harmless: the command is a level,
# not a toggle, and it survives the DDS discovery delay of a fresh container)
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: true}'"
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: false}'"
```

Each `docker run` is a brand-new DDS participant, so the agent needs a few seconds to
discover it. `ros2 topic pub --once` can therefore be dropped before the match; the
`-r 2 --times 5` form publishes over ~2.5 s and always gets through.

## 5. Low-latency control (sub-100 ms)

The commands above are fine for checking that everything works, but every one of them
pays the same fixed cost before the board hears anything:

| Step | Cost on a Pi 3B+ |
|---|---|
| `docker run` (container create/start) | ~0.5-1 s |
| Python + `ros2` CLI import | ~2-4 s |
| New DDS participant, agent must discover it | ~0.2-1 s |
| `--once`/`--times` waits for that match | up to seconds |

None of that is the board: once the message reaches the agent, the firmware reacts in a
couple of milliseconds. The fix is to stop paying the setup cost per command.

The board side was tightened as well. The main loop used to run
`rclc_executor_spin_some(10 ms)` followed by `k_msleep(10)`, i.e. up to ~20 ms of extra
delay between "sample arrived on the socket" and "callback set the LED". It now spins
with a 1 ms wait and a 1 ms sleep, so the callback runs within ~1-2 ms. That is noise next
to the seconds spent in `docker run`, but it removes the last avoidable part.

`docker-compose.yml` includes an optional `led-ctl` service that starts **once**, keeps
a publisher that is already matched with the agent, and watches a file:

```bash
cd raspberry_pi_agent              # from the repository root
docker compose up -d led-ctl
docker logs -f led-ctl            # "watching /ctrl/led ..."

echo true  > ctrl/led             # blue LED on  (~tens of ms)
echo false > ctrl/led             # blue LED off
```

The value is re-published every 2 s, so the LED also converges after a board reset
without you resending anything. Accepted words: `true/false`, `on/off`, `1/0`.

### Would Cyclone DDS help?

Short answer: not on the agent side, and only marginally on the client side.

- The micro-ROS agent image is built against **Fast DDS**. Its source has exactly two
  middleware backends, `fastdds` and a test backend (`ced`); there is no Cyclone build, so
  the board always talks to a Fast DDS participant.
- What you *can* switch is the ROS 2 process on the Pi (the `ros2` CLI, or `led-ctl`) with
  `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp`. It then interoperates with the agent over RTPS:
  cross-vendor interop is standard, but discovery is slower and less forgiving than
  same-vendor, so it is a change to test, not a guaranteed improvement.
- The gain is small because DDS is not where the seconds go: starting a container (~1 s)
  and starting Python + rclpy (~2-4 s on a Pi 3B+) dominate. Cyclone's participant startup
  is somewhat lighter than Fast DDS's, so expect a few hundred milliseconds at best.
- With `led-ctl` the participant is created once at `docker compose up`, so the vendor
  hardly matters: publishing the sample takes milliseconds either way.
- Side effect: Cyclone does not emit the Fast DDS `sequence size exceeds remaining buffer`
  warnings, because those come from the Fast DDS 2.6 CLI talking to the newer Fast DDS in
  the agent.

Measure before changing anything — these three commands tell you where the time goes:

```bash
# floor: container start only
time docker run --rm --network host ros:humble true

# floor: Python + rclpy import (no DDS participant yet)
time docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && python3 -c 'import rclpy'"

# the led-ctl path (persistent publisher, no per-command startup)
time bash -c 'echo true > ctrl/led'
```

If Cyclone is in the image (empty output means it is not):

```bash
docker run --rm ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 pkg list | grep rmw_cyclonedds"
```

then run the same publish with and without it and compare:

```bash
time docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: true}'"

time docker run --rm --network host -e RMW_IMPLEMENTATION=rmw_cyclonedds_cpp ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: false}'"
```

To use it permanently for `led-ctl`, uncomment the `RMW_IMPLEMENTATION` line in
`docker-compose.yml` (install `ros-humble-rmw-cyclonedds-cpp` first if the check above came
back empty).

If you prefer to keep the `ros2` CLI, add `--ipc=host` (the compose file already sets
`ipc: host` for the agent) so Fast DDS can use shared memory instead of falling back to
UDP. It does not remove the Python/container startup cost, but it removes the discovery
stalls:

```bash
docker run --rm --network host --ipc=host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: true}'"
```

> `osrf/ros:humble-desktop` is **amd64-only** and fails on the Pi with
> `exec format error`. `ros:humble` is published for arm64 and works.
> The first pull is ~700 MB, so give the Pi a few minutes.

> The Pi CPU is slow - first pulls/runs take a while, `ros2 topic pub -r 10` is plenty
> for demos (leave `-r` default of 1 Hz for the LED command).

## 6. Notes & troubleshooting

- **ROS_DOMAIN_ID** must match on both ends (default `0`). If you change it, export
  `ROS_DOMAIN_ID=<id>` in the agent service (`environment:` in docker-compose) and in
  every ros2 test container.
- **No topics?** Check that the Nucleo's `CONFIG_MICROROS_AGENT_IP` is this Pi's IP,
  port `8888`, that both are on the same subnet, and that no firewall blocks UDP 8888
  (`sudo netstat -lu | grep 8888` on the Pi should show the listener).
- **Nucleo console stuck on "Waiting..."**: the firmware prints the transport state
  (`net: ... is 192.168.x.y`, `udp: socket ... connected`, `udp: connect() ... failed`,
  `udp: send() ... failed`). `connect()` before DHCP is retried safely; if it keeps
  failing after the board has an IP, the Pi is not reachable at the configured address.
  On the Pi, run `sudo tcpdump -i any host <nucleo-ip> -n` and reset the board to see
  whether any packets leave it.
- **Serial instead of UDP (alternative)**: run the agent with
  `serial --dev /dev/ttyACM0 -v6` inside a container that mounts `/dev`
  (`--privileged -v /dev:/dev`). Then the Nucleo must be switched to the serial
  transport (see the Nucleo README). UDP is the default here - it needs no USB cable.
- **After reboots**: the compose service has `restart: unless-stopped`, so the agent
  comes back automatically. The Nucleo retries pinging until the agent is there.
- **RMW**: the agent is Fast DDS (the image has no Cyclone backend). The `ros2` CLI and
  `led-ctl` default to Fast DDS too; switching only those to Cyclone
  (`RMW_IMPLEMENTATION=rmw_cyclonedds_cpp`) is possible but works over cross-vendor RTPS —
  see section 5. Don't mix RMW settings *between* the CLI containers and the agent without
  testing, and keep `ROS_DOMAIN_ID` identical.
