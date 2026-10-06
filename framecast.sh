#!/bin/bash
# FrameCast: cast the Steam Frame headset view to a Chromecast.
#
#   framecast.sh start [device-name]   start casting in the background (systemd user unit "framecast")
#   framecast.sh stop                  stop casting and stop the Chromecast
#   framecast.sh status                show the unit status and the last log lines
#   framecast.sh list                  list Chromecasts on the network
#   framecast.sh level on|off          keep the horizon level on the TV (also during a running cast)
#   framecast.sh run [device-name]     run in the foreground (what the unit runs)
#
# Settings come from ~/FrameCast/framecast.conf (see framecast.conf.example).
# The current state is written to $XDG_RUNTIME_DIR/framecast.state for the dashboard panel (framecast-ui).
set -u

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BIN="$HERE/bin"
UNIT=framecast

DEVICE="Living Room"   # Chromecast device name, as shown by "framecast.sh list"
MODE=stream            # stream = Cast Streaming (mirroring receiver, ~0.3 s delay); hls = HLS (~6 s delay)
WIDTH=1280
HEIGHT=720
FPS=30
BITRATE=4M
DELAY=120              # stream mode: receiver playout delay in ms; lower = less delay, more stutter on bad Wi-Fi
AUDIO=1                # 1 = also send the headset audio (Opus in stream mode, AAC in hls mode)
AUDIO_BITRATE=128k
ENCODER=hw             # stream mode: hw = hardware H.264 encoder (hwenc), x264 = software encoder
PORT=8090
SEGMENT=1              # HLS segment length in seconds; lower = less delay, higher = fewer stalls
PLAYLIST=5             # segments in the live playlist; the Chromecast starts at the oldest one
STAB_ZOOM=1.2          # zoom while the horizon is kept level (hides the rotated corners for small tilts)
HLS_DIR=/tmp/framecast
STAB_FILE="$HERE/stabilize"   # "1" = keep the horizon level; the panel writes it, framecap reads it live
STATE_FILE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/framecast.state"
[ -f "$HERE/framecast.conf" ] && . "$HERE/framecast.conf"

log() { echo "framecast: $*" >&2; }

# set_state <idle|starting|casting|error> [message]
set_state() {
    printf 'state=%s\ndevice=%s\npid=%s\nmessage=%s\n' "$1" "$DEVICE" "$$" "${2:-}" > "$STATE_FILE.tmp"
    mv -f "$STATE_FILE.tmp" "$STATE_FILE"
}

state_device() {
    sed -n 's/^device=//p' "$STATE_FILE" 2>/dev/null
}

fail() {
    log "$1"
    FAILED=1
    set_state error "$1"
    exit 1
}

local_ip() {
    ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p'
}

# "4M" -> 4000000
to_bps() {
    case "$1" in
        *[Mm]) echo $(( ${1%?} * 1000000 )) ;;
        *[Kk]) echo $(( ${1%?} * 1000 )) ;;
        *) echo "$1" ;;
    esac
}

# Address (ip:port) of a Cast device by its name.
device_address() {
    "$BIN/go-chromecast" ls --dns-timeout 4 2>/dev/null |
        grep -F "device_name=\"$1\"" | head -1 | sed -n 's/.* address="\([^"]*\)".*/\1/p'
}

# Low-delay path: framecap -> x264 -> framecast_sender (Cast Streaming to the built-in mirroring app).
run_stream() {
    FAILED=0
    set_state starting
    local addr; addr="$(device_address "$DEVICE")"
    [ -n "$addr" ] || fail "Cannot find '$DEVICE' on the network"
    local status="${STATE_FILE%.state}.sender"
    rm -f "$status"

    cleanup() {
        trap - EXIT INT TERM
        log "stopping"
        # framecast_sender stops the mirroring app on the TV when it gets SIGTERM.
        pkill -TERM -P $$ 2>/dev/null
        wait
        rm -f "$status" "${STATE_FILE%.state}.key" "${STATE_FILE%.state}.audio"
        [ "$FAILED" = 1 ] || set_state idle
    }
    trap cleanup EXIT
    trap 'cleanup; exit 0' INT TERM

    local bps; bps="$(to_bps "$BITRATE")"
    log "streaming to '$DEVICE' ($addr), delay ${DELAY} ms, encoder $ENCODER, audio $AUDIO"
    # framecast_sender writes a byte to this FIFO when the TV needs a key frame; hwenc then makes
    # the next frame an IDR. With x264 nothing reads it and the picture waits for the next IDR (1 s).
    local keyfifo="${STATE_FILE%.state}.key"
    rm -f "$keyfifo"; mkfifo "$keyfifo"
    local sender_args=(-a -m "$bps" -W "$WIDTH" -H "$HEIGHT" -f "$FPS" -D "$DELAY" -s "$status" -K "$keyfifo")
    local audiofifo="${STATE_FILE%.state}.audio"
    rm -f "$audiofifo"
    if [ "$AUDIO" = 1 ]; then
        # 10 ms Opus packets, one Ogg page each, flushed at once: framecast_sender reads them live.
        mkfifo "$audiofifo"
        ffmpeg -hide_banner -loglevel warning -nostdin \
            -f pulse -fragment_size 3840 -i @DEFAULT_MONITOR@ -ac 2 -ar 48000 \
            -c:a libopus -b:a "$AUDIO_BITRATE" -application lowdelay -frame_duration 10 \
            -page_duration 10000 -flush_packets 1 -f ogg - > "$audiofifo" &
        sender_args+=(-B "$(to_bps "$AUDIO_BITRATE")" -A "$audiofifo")
    fi
    encode() {
        if [ "$ENCODER" = x264 ]; then
            # One IDR per second so the picture recovers fast after a lost frame. aud=1 and
            # repeat-headers=1 are what framecast_sender needs to split the stream into frames.
            exec ffmpeg -hide_banner -loglevel warning -nostdin \
                -f rawvideo -pix_fmt rgba -s "${WIDTH}x${HEIGHT}" -framerate "$FPS" -i - \
                -c:v libx264 -preset ultrafast -tune zerolatency -threads 2 -pix_fmt yuv420p \
                -b:v "$bps" -maxrate "$bps" -bufsize "$((bps / FPS * 4))" -bf 0 \
                -g "$FPS" -keyint_min "$FPS" -sc_threshold 0 \
                -x264-params aud=1:repeat-headers=1 -an -f h264 -
        else
            # Key frames every 2 s, and at once when the TV asks for one.
            exec "$BIN/hwenc" -W "$WIDTH" -H "$HEIGHT" -f "$FPS" -b "$bps" -g "$((FPS * 2))" -k "$keyfifo"
        fi
    }
    "$BIN/framecap" --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" --control "$STAB_FILE" --zoom "$STAB_ZOOM" | \
        encode | \
        "$BIN/framecast_sender" "${sender_args[@]}" "$addr" &
    local pipe=$!

    # Follow the sender's status file.
    local st="" last=""
    while kill -0 "$pipe" 2>/dev/null; do
        st="$(cat "$status" 2>/dev/null)"
        if [ "$st" != "$last" ]; then
            last="$st"
            case "$st" in
                streaming) set_state casting ;;
                ended) fail "The TV ended the cast" ;;
                input-ended) fail "Capture stopped" ;;
                "failed launch"*) fail "'$DEVICE' cannot start screen mirroring" ;;
                "failed connect"*) fail "Could not reach '$DEVICE'" ;;
                "failed busy"*) fail "'$DEVICE' is busy with another sender" ;;
                "failed codec"*) fail "'$DEVICE' does not accept H.264 video" ;;
                failed*) fail "The cast stopped (${st#failed })" ;;
            esac
        fi
        sleep 0.5
    done
    st="$(cat "$status" 2>/dev/null)"
    case "$st" in
        ended) fail "The TV ended the cast" ;;
        failed*) fail "The cast stopped (${st#failed })" ;;
        *) fail "Capture stopped" ;;
    esac
}

run() {
    [ $# -ge 1 ] && DEVICE="$1"
    [ "$MODE" = hls ] || { run_stream; return; }
    FAILED=0
    CAST_URL=""
    set_state starting
    local ip; ip="$(local_ip)"
    [ -n "$ip" ] || fail "No network connection"

    # A new path for each cast: the Chromecast caches playlists and segments by URL.
    local session; session="$(date +%s)"
    local out="$HLS_DIR/$session"
    rm -rf "$HLS_DIR"; mkdir -p "$out"
    local pids=()
    cleanup() {
        trap - EXIT INT TERM
        log "stopping"
        # All children (capture, encoder, server). Not "${pids[@]}": locals are gone when the EXIT trap runs.
        pkill -TERM -P $$ 2>/dev/null
        # Stop the Chromecast only if it still plays this cast, not when someone uses it for something else.
        if [ -n "$CAST_URL" ] && timeout 10 "$BIN/go-chromecast" -n "$DEVICE" status 2>/dev/null | grep -qF "$CAST_URL"; then
            timeout 10 "$BIN/go-chromecast" -n "$DEVICE" stop >/dev/null 2>&1
        fi
        wait
        rm -rf "$HLS_DIR"
        [ "$FAILED" = 1 ] || set_state idle
    }
    trap cleanup EXIT
    trap 'cleanup; exit 0' INT TERM

    python3 "$HERE/src/castserver.py" "$HLS_DIR" "$PORT" &
    pids+=($!)

    local audio_in=() audio_out=(-an)
    if [ "$AUDIO" = 1 ]; then
        audio_in=(-thread_queue_size 1024 -f pulse -i @DEFAULT_MONITOR@)
        audio_out=(-c:a aac -b:a 128k -ac 2)
    fi

    # Keyframe at every segment start so the receiver can join at any segment.
    local gop=$((FPS * SEGMENT))
    "$BIN/framecap" --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" --control "$STAB_FILE" --zoom "$STAB_ZOOM" | \
        ffmpeg -hide_banner -loglevel warning -nostdin \
            -thread_queue_size 64 -f rawvideo -pix_fmt rgba -s "${WIDTH}x${HEIGHT}" -framerate "$FPS" -i - \
            "${audio_in[@]}" \
            -c:v libx264 -preset ultrafast -tune zerolatency -threads 2 -pix_fmt yuv420p \
            -profile:v high -level 4.1 -b:v "$BITRATE" -maxrate "$BITRATE" -bufsize "$BITRATE" \
            -g "$gop" -keyint_min "$gop" -sc_threshold 0 \
            "${audio_out[@]}" \
            -f hls -hls_time "$SEGMENT" -hls_list_size "$PLAYLIST" -hls_flags delete_segments+independent_segments \
            -hls_segment_filename "$out/seg%05d.ts" "$out/stream.m3u8" &
    pids+=($!)

    # Start the Chromecast only when the playlist holds a few segments, else it gives up at once.
    local url="http://$ip:$PORT/$session/stream.m3u8"
    CAST_URL=""
    local segs
    for _ in $(seq 60); do
        segs="$(grep -c '\.ts$' "$out/stream.m3u8" 2>/dev/null)"
        [ "${segs:-0}" -ge "$PLAYLIST" ] && break
        kill -0 "${pids[1]}" 2>/dev/null || fail "Capture or encoder failed"
        sleep 1
    done
    log "casting $url to '$DEVICE'"
    load() { timeout 30 "$BIN/go-chromecast" -n "$DEVICE" load "$url" -c application/x-mpegURL --detach; }
    CAST_URL="$url"
    load || fail "Could not reach '$DEVICE'"
    set_state casting

    # Watchdog. The Chromecast sometimes never starts the stream (player open but idle, or BUFFERING for
    # good); then send the stream again. When the TV shows another app or its idle screen, the cast was
    # ended on the TV side.
    local stuck=0 nomedia=0 gone=0 reloads=0 st
    while true; do
        for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null || fail "Capture stopped"; done
        sleep 5 & wait $!
        st="$(timeout 10 "$BIN/go-chromecast" -n "$DEVICE" status 2>/dev/null | head -1)"
        case "$st" in
            *"(PLAYING)"*"$url"*) stuck=0; nomedia=0; gone=0; reloads=0 ;;
            *"$url"*) stuck=$((stuck + 1)); nomedia=0; gone=0 ;;
            *"Default Media Receiver"*) nomedia=$((nomedia + 1)); gone=0 ;;
            "") ;;  # No answer this time; ask again.
            *) gone=$((gone + 1)) ;;
        esac
        [ "$gone" -ge 3 ] && fail "The TV ended the cast"
        if [ "$stuck" -ge 4 ] || [ "$nomedia" -ge 2 ]; then
            reloads=$((reloads + 1))
            [ "$reloads" -gt 5 ] && fail "The Chromecast does not play the stream"
            log "Chromecast not playing ($st); loading the stream again ($reloads)"
            load || log "reload failed"
            stuck=0; nomedia=0
        fi
    done
}

case "${1:-}" in
    start)
        shift
        systemctl --user stop "$UNIT" 2>/dev/null
        systemctl --user reset-failed "$UNIT" 2>/dev/null
        systemd-run --user --unit="$UNIT" --collect --quiet "$HERE/framecast.sh" run "$@"
        log "started (see: framecast.sh status)"
        ;;
    stop)
        if systemctl --user is-active --quiet "$UNIT"; then
            systemctl --user stop "$UNIT"   # the unit stops the Chromecast itself
        else
            dev="$(state_device)"; DEVICE="${dev:-$DEVICE}"
            set_state idle
        fi
        log "stopped"
        ;;
    toggle)
        shift
        if systemctl --user is-active --quiet "$UNIT"; then "$0" stop; else "$0" start "$@"; fi
        ;;
    status)
        systemctl --user status "$UNIT" --no-pager -n 0 2>/dev/null | head -4
        journalctl --user -u "$UNIT" -n 15 --no-pager -o cat 2>/dev/null
        ;;
    list)
        "$BIN/go-chromecast" ls
        ;;
    level)
        case "${2:-}" in on) echo 1 > "$STAB_FILE" ;; off) echo 0 > "$STAB_FILE" ;; *) cat "$STAB_FILE" 2>/dev/null || echo 0 ;; esac
        ;;
    run)
        shift
        run "$@"
        ;;
    *)
        sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
