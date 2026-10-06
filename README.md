# FrameCast

Cast the **Steam Frame** headset view (standalone mode) straight to a **Chromecast**, including old
cast-only dongles that cannot run the Steam Link app. No phone or PC in the middle, about 0.2–0.3 s
delay, with sound, and an optional "keep the horizon level" mode for viewers.

It uses the same Cast Streaming protocol as Chrome's tab casting: the Frame's hardware H.264 encoder
feeds an [Open Screen](https://chromium.googlesource.com/openscreen) sender that talks to the TV's
built-in screen-mirroring receiver. A SteamVR dashboard panel picks the TV and shows the cast state.

> Unofficial hobby project. Not affiliated with Valve or Google. Tested on one Steam Frame (SteamOS,
> kernel 6.18, `qcom-iris` encoder) and a classic Chromecast dongle (firmware 1.56).

## Requirements

- A Steam Frame with developer mode / adb access, on the same Wi-Fi as the Chromecast.
- A Linux PC with `adb`, `curl` and `git`.
- On the Frame (all present on SteamOS for the Frame): `gcc`/`g++`, `glslc`, `ffmpeg` (with libopus and
  PulseAudio input), cairo, Vulkan, SteamVR (`/opt/steamvr`).

## Install

```sh
git clone https://github.com/nickgkg/FrameCast.git
cd FrameCast
./install.sh
```

`install.sh` downloads `framecast_sender` (from this repository's latest release) and go-chromecast (from
its release) into `bin/`, pushes everything to `~/FrameCast/` on the Frame over adb, builds the other
programs there, and starts the dashboard panel service. Run it again after any change.

## Use

In the headset: open the SteamVR dashboard → **Cast to TV** panel.

- Click **Cast here** on a Chromecast. Status goes yellow ("Starting…"), then green after ~3 s.
- **Keep the horizon level** switch: the TV picture turns against your head roll, so the horizon stays
  level (zoomed 1.2× to hide the corners at small tilts; `STAB_ZOOM`). Works during a running cast, and
  fades out when you look almost straight up or down. The choice is kept in `~/FrameCast/stabilize`.
- Click **Stop** to end it. A headset notification shows on start, stop and errors.
- The list refreshes each time the panel opens (if older than 15 s) and with **Refresh**, so any
  Chromecast on the current Wi-Fi shows up by its normal name. The last used one is listed first.
- Audio-only Cast devices (speakers) are listed too; they cannot show video.

Over adb (`~/FrameCast/framecast.sh`): `start "<name>"`, `stop`, `status`, `list`, `level on|off`.

Settings are in `~/FrameCast/framecast.conf` on the Frame (see `framecast.conf.example`): bitrate, size,
frame rate, `DELAY` (TV playout buffer, default 120 ms), `AUDIO`, `ENCODER=hw|x264`, and `MODE=hls` for
the old HLS path (~6 s delay, works with the Default Media Receiver).

## How it works

Default (`MODE=stream`), Cast Streaming to the TV's built-in screen-mirroring receiver (app `0F5096E8`,
the one Chrome tab casting uses):

```
framecap ──raw RGBA──▶ hwenc (iris H.264) ──stdin──▶ framecast_sender ──RTP/UDP (AES)──▶ Chromecast :10008
   ▲                       ▲                            ▲  │ CASTV2/TLS :8009: LAUNCH, OFFER/ANSWER, PING, STOP
SteamVR Headset View       └── key frame requests (FIFO) ┘  │
PulseAudio monitor ──▶ ffmpeg (Opus 10 ms, Ogg) ──FIFO──────┘
```

Fallback (`MODE=hls`):

```
framecap ──raw RGBA──▶ ffmpeg (x264) ──HLS──▶ castserver.py :8090 ──▶ Chromecast (Default Media Receiver)
```

| File | Role |
|---|---|
| `src/framecap.cpp` | Reads the SteamVR "Headset View" via `IVROverlayView::AcquireOverlayView` on overlay `system.HeadsetView` (works while hidden), GPU-blits to 1280×720, writes frames to stdout at a fixed rate. With stabilization on it uses the compute shader instead of the blit. `--test-roll DEG` forces a roll for testing (e.g. with `--overlay framecast.panel` as a known image). |
| `src/stabilize.comp` | Compute shader: rotate + zoom around the center, re-encode sRGB. Compiled on the Frame by `install.sh` (`glslc` is on the Frame, not the PC) into `src/stabilize_spv.h`. |
| `src/hwenc.c` | Hardware H.264 encoder (qcom-iris, `/dev/video-enc0`) through the V4L2 stateful encoder API. RGBA in (the encoder converts), Annex-B with AUDs out, ~4 ms per frame. `-k FIFO`: forces an IDR for each byte that framecast_sender writes there. `ENCODER=x264` uses software x264 instead. |
| `sender/` | `framecast_sender`: Open Screen (libcast) sender. Connects, launches the mirroring app, negotiates H.264 (+ Opus with `-A`), splits the H.264 stream on AUD NALs and sends each frame with `cast::Sender` (retransmits on NACK). Writes its state to `$XDG_RUNTIME_DIR/framecast.sender`. Cross-compiled on the PC by `sender/build-sender.sh` into `bin/framecast_sender`. `--vp8` reads IVF instead (test option). |
| `framecast.sh` | Runs the pipeline as the transient systemd user unit `framecast`, writes `$XDG_RUNTIME_DIR/framecast.state`. HLS mode also starts the Chromecast and runs a watchdog. |
| `src/castserver.py` | HLS mode: HTTP server for the HLS files (CORS headers, no-cache playlists). |
| `src/framecast-ui.cpp` | The dashboard panel (systemd user service `framecast-ui`, enabled). Cairo drawing, uploaded as Vulkan overlay textures. |
| `bin/go-chromecast` | [vishen/go-chromecast](https://github.com/vishen/go-chromecast) v0.3.4 arm64: discovery (both modes) and Cast control (HLS mode). |
| `framecast.conf` | Optional settings (mode, delay, encoder, audio, size, fps, bitrate; HLS: segment length). On the Frame only; `install.sh` copies `framecast.conf.example` once and never overwrites it. |

Run-time files in `$XDG_RUNTIME_DIR` (`/run/user/1000`): `framecast.state` (for the panel), and while a cast
runs `framecast.sender` (sender status), `framecast.key` and `framecast.audio` (FIFOs). The cleanup removes
all but `framecast.state`. Logs: `journalctl --user -u framecast` (the sender and hwenc log stats every 5 s).

## Building the sender

Only needed to change `sender/`; `install.sh` otherwise uses the release binary. From the PC:

1. `sender/build-sender.sh`. It fetches Open Screen into
   `~/src/openscreen-build` (needs an ext4 disk, ~5.5 GB; the Chromium sysroots have file names NTFS refuses)
   and cross-compiles `bin/framecast_sender` for arm64 with the Chromium clang and the Debian arm64 sysroot.
   The binary links libc++ statically and runs on the Frame's glibc. `sender/` is symlinked into the tree as
   `cast/framecast_sender`; `sender/openscreen-build.patch` is the only change to Open Screen itself. The tree
   can be deleted and fetched again by the script. For a quick rebuild after an edit:
   `cd ~/src/openscreen-build/openscreen && third_party/ninja/ninja -C out/arm64 framecast_sender`, then copy
   `out/arm64/framecast_sender` to `bin/`.
2. `./install.sh`. It pushes everything to `~/FrameCast/` on the Frame, builds `framecap`, `hwenc` and
   `framecast-ui` there with the on-device gcc/g++, and (re)starts `framecast-ui`. Binaries are replaced by
   rename, so a running cast keeps going (overwriting a running binary in place crashes it with SIGBUS).

## Findings

Notes from development, so that dead ends are not retried.

- `IVRCompositor::GetMirrorTextureGL` fails (102 InvalidTexture; GL is zink). `IVROverlay::GetOverlayTexture`
  on the Headset View fails (11). `AcquireOverlayView` is the route that works.
- Hardware H.264 encoder (`/dev/video-enc0`, mainline `qcom-iris`): ffmpeg 7.0 `h264_v4l2m2m` hangs or segfaults
  and keeps the device busy (needs `kill -9`); GStreamer `v4l2h264enc` fails to negotiate; `v4l2-ctl --stream-*`
  writes nothing. The driver itself works (`hwenc.c`): set the OUTPUT (raw) format first (the coded size follows
  from it; the height is padded to 736, so copy rows with the driver's stride and set a crop selection), then the
  CAPTURE format. It accepts RGBA (`AB24`) directly. An encode loop must not block on stdin while frames are in
  the encoder, or each frame waits for the next input (67 ms instead of 4 ms). Buffer timestamps must be real frame
  times (n × 1/fps): the driver's rate control takes the frame rate from them, and 1 µs steps gave a
  very blocky picture (~0.3 Mbit/s live at a 4 Mbit/s setting; with real times the output matches the setting). Steam's own V4L2 code
  (`streaming_client`, `CV4L2Accel`) only decodes.
- `SetOverlayFromFile` caches each path forever: a changing panel must use `SetOverlayTexture`.
- Overlay mouse events have their origin at the bottom left.
- HLS delay: segment 1 s × 5 in the playlist gives ~6 s. Shorter playlists or seeking to the live edge do not
  help; the Default Media Receiver keeps its own buffer. Hence the Cast Streaming mode.
- Cast Streaming on a classic Chromecast dongle (firmware 1.56):
  - With an audio stream of silent zero-length Opus frames, the Chromecast accepts the OFFER but never sends
    any RTCP for either stream, and the sender stalls. Real Opus packets (libopus, 10 ms, as Chrome sends) work.
  - Audio that collects in the FIFO while the session starts arrives in one burst; the sender drops packets
    that run more than 60 ms ahead of real time, or the TV plays the sound late.
  - Open Screen's sender has no CASTV2 heartbeat; without PING every 5 s the TV drops the connection.
  - `-a` (Chrome payload types 96/127) is used, as Chrome and other working senders do.
  - The STOP message must go out before the socket closes, and a second SIGTERM (systemd + script) must not
    kill the sender first; otherwise the TV stays on "Chrome Mirroring".
  - Playout delay: with x264 and a fixed IDR each second, 150 ms gave up to ~1 s of dropped video on Wi-Fi
    hiccups (the sender waits for the next IDR). With hwenc the sender asks for an IDR at once (`-K` FIFO, on
    a drop or on receiver picture loss), so a hiccup costs ~0.3 s at most. 120 ms: ~0.7 % frames dropped in
    short bursts, same as 250 ms, so 120 ms is the default. RTT on the LAN is 3–15 ms.
  - On SIGTERM the encoder exits at the same time, so the agent also shuts down by itself ("input-ended").
    That path must not end the stop sequence early (it destroyed the agent after the task runner and crashed
    with SIGSEGV after STOP was sent). `main.cc` ignores the agent's shutdown callback once stopping.
  - Each crash of a FrameCast program leaves a minidump in `/var/lib/steamos-log-submitter/pending/minidump/`
    (root-owned; the SteamOS log submitter sends it to Valve) and a core in `coredumpctl`.
  - The reference [classic-chromecast-caster](https://github.com/RikvdReijen/classic-chromecast-caster)
    (C#) works video-only on the same TV; its protocol library runs on Linux with `dotnet build -p:LangVersion=preview`.
- HLS mode: the Chromecast sometimes opens the player but never starts the stream, or stays in BUFFERING. The watchdog
  in `framecast.sh` loads the stream again (after 10 s / 20 s, at most 5 times). Each cast uses a new URL
  path because the receiver caches by URL.
- When the TV shows another app or its idle screen for 15 s, the cast ends with "The TV ended the cast",
  and the Chromecast is not stopped (so it does not kill someone else's YouTube).
- Roll is `atan2(m[1][0], m[1][1])` of the HMD pose (positive = head tilted left); the image turns
  counterclockwise by that angle. Confirmed correct in the headset. The Headset View is not smoothed, so the
  raw pose matches it with no visible wobble.

## Ideas not done

- Adaptive bitrate from RTCP feedback; try `DELAY` below 120.
- Hide audio-only devices (needs the Cast mDNS `ca` capability bits; go-chromecast does not show them).

## License

MIT, see `LICENSE`. Parts of `sender/` are based on Open Screen (BSD-style); see `THIRD_PARTY_NOTICES.md`.
