#!/bin/bash
# Push FrameCast to the Steam Frame over adb, build it on the device, and start the dashboard panel.
set -euo pipefail
cd "$(dirname "$0")"

DEST=/home/steamos/FrameCast
GO_CHROMECAST_URL=https://github.com/vishen/go-chromecast/releases/download/v0.3.4/go-chromecast_Linux_arm64.tar.gz
FRAMECAST_SENDER_URL=https://github.com/nickgkg/FrameCast/releases/latest/download/framecast_sender-linux-arm64
VRLIB=/opt/steamvr/bin/linuxarm64

if [ ! -x bin/go-chromecast ]; then
    mkdir -p bin
    curl -sL "$GO_CHROMECAST_URL" | tar xz -C bin go-chromecast
fi

# Prebuilt sender from the latest GitHub release, unless you built your own (sender/build-sender.sh).
if [ ! -x bin/framecast_sender ]; then
    mkdir -p bin
    curl -fsSL -o bin/framecast_sender "$FRAMECAST_SENDER_URL" || {
        echo "Cannot download framecast_sender; build it with sender/build-sender.sh" >&2
        rm -f bin/framecast_sender
        exit 1
    }
    chmod +x bin/framecast_sender
fi

adb shell mkdir -p "$DEST/bin" "$DEST/src" /home/steamos/.config/systemd/user
adb push src/framecap.cpp src/stabilize.comp src/framecast-ui.cpp src/openvr.h src/castserver.py src/hwenc.c "$DEST/src/" >/dev/null
# Push to new names and rename: overwriting a running binary in place crashes it (SIGBUS).
adb push bin/go-chromecast "$DEST/bin/go-chromecast.new" >/dev/null
adb push bin/framecast_sender "$DEST/bin/framecast_sender.new" >/dev/null
adb shell "cd $DEST/bin && mv -f go-chromecast.new go-chromecast && mv -f framecast_sender.new framecast_sender"
adb push framecast.sh framecast.conf.example "$DEST/" >/dev/null
adb push framecast-ui.service /home/steamos/.config/systemd/user/ >/dev/null
adb shell "cd $DEST && chmod +x framecast.sh bin/go-chromecast bin/framecast_sender && \
    glslc -O -mfmt=c src/stabilize.comp -o src/stabilize_spv.h && \
    gcc -O2 -Wall src/hwenc.c -o bin/hwenc.new && mv -f bin/hwenc.new bin/hwenc && \
    g++ -O2 -std=c++17 -Isrc src/framecap.cpp -o bin/framecap.new \
        -L$VRLIB -Wl,-rpath,$VRLIB -lopenvr_api -lvulkan && mv -f bin/framecap.new bin/framecap && \
    g++ -O2 -std=c++17 src/framecast-ui.cpp -o bin/framecast-ui \$(pkg-config --cflags --libs cairo) \
        -L$VRLIB -Wl,-rpath,$VRLIB -lopenvr_api -lvulkan -lpthread && \
    { [ -f framecast.conf ] || cp framecast.conf.example framecast.conf; } && \
    systemctl --user daemon-reload && \
    systemctl --user enable framecast-ui.service >/dev/null 2>&1 && \
    systemctl --user restart framecast-ui.service"
echo "installed to $DEST; the 'Cast to TV' panel is in the SteamVR dashboard"
