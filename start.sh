#!/bin/sh
# Starts (or restarts) Paper DJ on the Pi. Runs at boot, and is safe to run by hand any time:
#   sh ~/paper-dj/start.sh
# Runs the engine as root (via sudo when started by hand) so it can create /dev/paperdj.

# ---- what to load --------------------------------------------------------
A_SONG=songs/A_stardust_msbwy.wav
A_BPM=124                       # leave empty to auto-detect
B_SONG=songs/B_john_summit_shiver.wav
B_BPM=
SAMPLE1=tracks/sample1_airhorn.wav
SAMPLE2=tracks/sample2_zap.wav
VISION_CMD=                     # e.g. "$DIR/vision/build/paper_vision"  (empty = don't start vision)
# --------------------------------------------------------------------------

DIR=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" != 0 ]; then
    exec sudo sh "$DIR/start.sh" "$@"
fi

slay -f dj_engine >/dev/null 2>&1
mkdir -p "$DIR/logs"
cd "$DIR/audio" || exit 1

set -- -a "$A_SONG" -b "$B_SONG" -1 "$SAMPLE1" -2 "$SAMPLE2"
[ -n "$A_BPM" ] && set -- "$@" -A "$A_BPM"
[ -n "$B_BPM" ] && set -- "$@" -B "$B_BPM"
nohup ./dj_engine "$@" > "$DIR/logs/engine.log" 2>&1 < /dev/null &

if [ -n "$VISION_CMD" ]; then
    slay -f "$(basename "$VISION_CMD")" >/dev/null 2>&1
    nohup $VISION_CMD > "$DIR/logs/vision.log" 2>&1 < /dev/null &
fi

# When run by hand, wait for the engine to load the songs and show the result.
if [ -t 1 ]; then
    sleep 8
    grep -E "deck|FIFO|resmgr" "$DIR/logs/engine.log"
    echo "Engine running. Open http://$(ifconfig bcm0 | awk '/inet /{print $2}'):8080/"
fi
