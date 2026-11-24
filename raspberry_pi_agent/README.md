# micro-ROS agent + ROS 2 on Raspberry Pi 3B+

The Pi runs the **micro-ROS agent**. It speaks XRCE-DDS with the Nucleo over UDP
(port 8888) and plain ROS 2 DDS on the other side, so the Nucleo shows up as an
ordinary node in the ROS 2 graph.

```
ros2 CLI (Docker, host net)  <--DDS-->  micro-ROS agent (Docker, UDP:8888)  <--UDP-->  Nucleo F767ZI
```

## 1. Prepare the Pi (one-time)

1. Flash a **64-bit** OS image (Raspberry Pi OS 64-bit or Ubuntu Server 24.04). The
   agent image is arm64-only.
2. Connect the Pi's Ethernet to the same switch as the Nucleo.
3. Install Docker:

   ```bash
   curl -fsSL https://get.docker.com | sudo sh
   sudo usermod -aG docker $USER
   # re-login afterwards
   ```

4. Note the Pi's IP address; the firmware has to point at it:

   ```bash
   hostname -I
   ```

   Put the value in `nucleo_f767zi_microros/prj.conf` as
   `CONFIG_MICROROS_AGENT_IP="..."`.

## 2. Start the agent

With compose (survives reboots):

```bash
cd raspberry_pi_agent   # copy this folder onto the Pi
docker compose up -d
```

or with the helper script (`chmod +x run-agent.sh`, then `./run-agent.sh`).

Equivalent one-liner:

```bash
docker run -d --name microros-agent --restart unless-stopped --net=host --privileged \
  -v /dev:/dev microros/micro-ros-agent:kilted udp4 --port 8888 -v4
```

The image is published for `linux/amd64` and `linux/arm64`. The tag matches the
firmware's `micro_ros_zephyr_module` branch (both `kilted`). `-v4` is the debug level;
`-v6` also logs every XRCE packet, which is slow on an SD card.

## 3. Check it

```bash
docker logs -f microros-agent
```

Without a client the agent only prints that it is listening (`CLI ... port 8888`).
When the Nucleo connects there are session/client lines as well.

## 4. Use it: ros2 CLI in a container

The agent image has no `ros2` CLI. Use `ros:humble` (multi-arch) on the host network:

```bash
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic list"
# /zephyr_heartbeat
# /zephyr_led_cmd
# /parameters
```

LED on and off:

```bash
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: true}'"
docker run --rm --network host ros:humble \
  bash -c "source /opt/ros/humble/setup.bash && ros2 topic pub -r 2 --times 5 /zephyr_led_cmd std_msgs/msg/Bool '{data: false}'"
```

`--once` sends a single sample immediately. A new container is a new DDS participant
and the agent needs a moment to match it, so that sample can be lost. `-r 2 --times 5`
publishes for about 2.5 s and covers the discovery window. The command is a level, so
repeating it is harmless.

## 5. Low-latency control

Before the board sees anything, each `docker run` above pays for the same startup:

| Step | Pi 3B+ |
|---|---|
| container create/start | ~0.5-1 s |
| Python + `ros2` CLI import | ~2-4 s |
| new DDS participant + discovery | ~0.2-1 s |
| `--once`/`--times` waiting for the match | up to seconds |

The firmware is not part of this. A sample that reaches the agent is handled in
milliseconds.

`docker-compose.yml` has an optional `led-ctl` service for interactive use. It starts
once, keeps a publisher matched with the agent, and watches a file:

```bash
cd raspberry_pi_agent              # from the repository root
docker compose up -d led-ctl
docker logs -f led-ctl            # "watching /ctrl/led ..."

echo true  > ctrl/led             # blue LED on
echo false > ctrl/led             # blue LED off
```

It re-publishes the current value every 2 s, so the LED also returns to the requested
state after a board reset. Accepted values: `true/false`, `on/off`, `1/0`.

On the board the main loop polls every 1 ms (`rclc_executor_spin_some(1 ms)` +
`k_msleep(1)`). It used to poll every 10 ms plus a 10 ms sleep, which added up to ~20 ms
between a sample arriving and the callback running.

### DDS implementation

The agent only builds against Fast DDS (the source has a `fastdds` backend and an
experimental `ced` one), so the board always talks to Fast DDS. That means Cyclone DDS
can only be used on the CLI side, and switching is not worth it here: the delay is
container and Python startup, which stays the same, and `led-ctl` creates its DDS
participant once anyway. `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp` works if you want to
try it (install `ros-humble-rmw-cyclonedds-cpp` in the image first), and it does silence
the `sequence size exceeds remaining buffer` warnings, which come from Fast DDS 2.6 in
`ros:humble` talking to the newer Fast DDS in the agent.

`--ipc=host` (set in `docker-compose.yml` for both services) lets Fast DDS use shared
memory between the containers instead of UDP.

> `osrf/ros:humble-desktop` is amd64-only and fails on the Pi with `exec format error`.
> `ros:humble` is published for arm64. The first pull is ~700 MB.

## 6. Notes & troubleshooting

- **ROS_DOMAIN_ID** must match on both sides (default `0`). If you change it, set
  `ROS_DOMAIN_ID` in the agent service (`environment:` in docker-compose) and in every
  `ros2` test container.
- **No topics**: check that `CONFIG_MICROROS_AGENT_IP` is this Pi's IP and that port
  `8888` is open (`sudo netstat -lu | grep 8888`).
- **Nucleo console stuck on "Waiting..."**: the firmware prints the transport state
  (`net: ... is 192.168.x.y`, `udp: socket ... connected`, `udp: connect() ... failed`,
  `udp: send() ... failed`). If `connect()` keeps failing after the board has an IP, the
  Pi is not reachable at that address. `sudo tcpdump -i any host <nucleo-ip> -n` on the
  Pi shows whether packets arrive.
- **Serial instead of UDP**: run the agent with `serial --dev /dev/ttyACM0 -v4` in a
  container with `--privileged -v /dev:/dev`, and switch the Nucleo to the serial
  transport (see the Nucleo README).
- **After a reboot**: `restart: unless-stopped` brings the agent back; the Nucleo keeps
  pinging until it answers.
