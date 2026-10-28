# gnd-vstreamer-mcsink

Ground-station receiver for the ground P4's video multicast, with an OSD. It
runs on the GS host (Rockchip, e.g. the opi5).

```text
rx:      multicast_source -> rtp_h264_depay -> h264_decoder_mpp -> osd_overlay -> queue
display: queue -> sdl_sink (KMSDRM on the console, or a window)
```

The ground P4 (gnd-esp32p4) recovers FEC and multicasts RTP H.264 to
`239.255.42.1:5600` (TTL 1) on its Ethernet. `rtp_h264_depay`,
`h264_decoder_mpp` and `sdl_sink` come from a pinned
[vstreamer](https://github.com/FlyingComputer2/vstreamer). `multicast_source`
and `osd_overlay` are this repo's own components.

## OSD

Top left, refreshed once a second:

```text
5994 kbps, 84.512 ms, 29.9 fps
```

- **kbps:** RTP bytes received per second.
- **ms:** mean camera-to-decode latency over the second. It is this host's
  monotonic time when the decoder outputs a frame, minus the frame's capture
  instant. The rover's packer writes that instant as `CLOCK_REALTIME` (RTP
  header extension id 2), and `rtp_h264_depay` maps it to this host's clock.
  `---` means no frame in the second had a plausible capture time.
- **fps:** frames out of the decoder per second.

The latency is only right if the clocks agree:

- The rover follows the ground P4 (winject-l3 app_synch).
- The ground P4 follows this host's chrony over SNTP (`sntp_server` in
  gnd-esp32p4 `main/main.cpp`).

So run chrony here, with `allow` for the GS LAN. The opi5's
`/etc/chrony/chrony.conf` already has `allow 192.168.253.0/24`.

## Build

Needs CMake ≥ 3.16, a C++17 compiler, Rockchip MPP (`librockchip_mpp`) and
SDL2:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

For a local vstreamer checkout, add `-DFETCHCONTENT_SOURCE_DIR_VSTREAMER=<dir>`.
For host tests (OSD text, latency window, NV12 burn-in), add
`-DMCSINK_BUILD_TESTS=ON`, then run `build/mcsink_tests`.

## Run

```sh
sudo build/gnd-vstreamer-mcsink --interface end1            # KMSDRM on the console
build/gnd-vstreamer-mcsink --interface end1 --display sdl   # window under X/Wayland
```

| Option | Default | |
|---|---|---|
| `--group` | `239.255.42.1` | multicast group |
| `--port` | `5600` | UDP port |
| `--interface` | routing choice | interface name or IPv4 address |
| `--display` | `kmsdrm` | `kmsdrm` or `sdl` |
| `--fps` | `30` | nominal frame rate |
| `--osd` | `1` | `0` turns off the burn-in (the stats lines stay) |
| `--show-corrupt` | `1` | show frames decoded after a loss, with artifacts; `0` drops them, freezing until the next IDR |
| `--stats` | `1` | seconds between stderr `MCSINK` lines, `0` = off |

As a service on the GS host (`deploy/gnd-vstreamer-mcsink.service`: runs as `ubuntu` on tty1, conflicts
with getty@tty1, restarts on exit; logs in `journalctl -u gnd-vstreamer-mcsink`):

```sh
sudo install -m 755 build-rel/gnd-vstreamer-mcsink /usr/local/bin/
sudo install -m 644 deploy/gnd-vstreamer-mcsink.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now gnd-vstreamer-mcsink
```

KMSDRM needs the console: stop getty on tty1, or any other KMS client, first.
Every `--stats` seconds it prints one key=value `MCSINK` line to stderr: the
OSD numbers (`latency_ms=-1.000` = no capture time in the window) and
counters. As a service, read it with
`journalctl -u gnd-vstreamer-mcsink -o cat | grep MCSINK`. Use this instead of
screenshots. With `SDL_VIDEODRIVER=offscreen` and `--display sdl` it runs
headless:

```text
MCSINK kbps=5994 latency_ms=84.512 screen_latency_ms=99.120 fps=29.90 received=… dropped=… rtp_lost=… capture_rejected=… corrupt_frames=… osd="drawn=… skipped=… …" display="…"
```

The OSD reads `<kbps> kbps, <latency> ms, <fps> fps, screen <ms> ms`. `latency` is
capture to decode. `screen` (`screen_latency_ms`) is capture to just after the
display's present (vstreamer `sdl_sink` `present_latency_ms`, last 1 s). With
vsync the page flip is queued then, so the frame is on screen up to one refresh
later, plus the monitor's own lag.

`dropped` counts datagrams this receiver's queue discarded. `rtp_lost` counts
RTP sequence gaps on arrival: loss on the radio after FEC, at the ground P4's
multicast fanout, or on the LAN. `corrupt_frames` counts frames the decoder
flagged as decoded against a lost reference (shown with `--show-corrupt 1`).
`osd` counts frames the OSD was drawn on and skipped (no caps yet, or a frame
smaller than its layout), with the caps and the last frame size.
