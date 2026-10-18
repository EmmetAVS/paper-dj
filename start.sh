#!/bin/sh
# Starts (or restarts) Paper DJ on the Pi: audio engine + paper board tracker. Runs at boot, and is
# safe to run by hand any time:
#   sh ~/paper-dj/start.sh                     songs set below
#   sh ~/paper-dj/start.sh deckA.wav deckB.wav other songs (paths relative to audio/, BPM auto-detected)
#   sh ~/paper-dj/start.sh stop                stop everything
# Runs as root (via sudo when started by hand) so the engine can create /dev/paperdj.
#
# Pages:  http://<pi>:8080  audio + test board (dj_engine)
#         http://<pi>:8081  board view: what the camera sees   (board_server.py)
#         http://<pi>:8082  same as 8080, also reachable over IPv6 (phone hotspots)

# ---- what to load --------------------------------------------------------
A_SONG=songs/A_stardust_msbwy.wav
A_BPM=124                       # leave empty to auto-detect
B_SONG=songs/B_john_summit_shiver.wav
B_BPM=
SAMPLE1=tracks/sample1_airhorn.wav
SAMPLE2=tracks/sample2_zap.wav
BOARD=1                         # 1 = start the camera tracker (board/deck) and board page if built
# --------------------------------------------------------------------------

PATH=/proc/boot:/system/bin:/system/sbin:/usr/bin:/usr/sbin:$PATH   # boot scripts get a bare PATH
export PATH
DIR=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" != 0 ]; then
    exec sudo sh "$DIR/start.sh" "$@"
fi

stop_all() {
    for p in $(pidin arg 2>/dev/null | grep board_server.py | grep -v grep | awk '{print $1}'); do kill "$p" 2>/dev/null; done
    slay -f deck >/dev/null 2>&1
    slay -f dj_engine >/dev/null 2>&1
    sleep 1
}
stop_all
if [ "$1" = stop ]; then echo "Paper DJ stopped."; exit 0; fi

if [ -n "$1" ]; then A_SONG=$1; A_BPM=; fi
if [ -n "$2" ]; then B_SONG=$2; B_BPM=; fi

mkdir -p "$DIR/logs"
cd "$DIR/audio" || exit 1
set -- -a "$A_SONG" -b "$B_SONG" -1 "$SAMPLE1" -2 "$SAMPLE2"
[ -n "$A_BPM" ] && set -- "$@" -A "$A_BPM"
[ -n "$B_BPM" ] && set -- "$@" -B "$B_BPM"
nohup ./dj_engine "$@" > "$DIR/logs/engine.log" 2>&1 < /dev/null &

# Camera tracker. board_server.py runs deck (restarting it if it exits), serves the board view on
# :8081 and relays the engine page on :8082. deck sends its commands to /dev/paperdj itself (DJ=off).
if [ "$BOARD" = 1 ] && [ -x "$DIR/board/deck" ]; then
    i=0; while [ ! -e /dev/paperdj ] && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done   # engine loads songs first
    slay -f camera_example3_viewfinder >/dev/null 2>&1
    cd "$DIR/board" && TRACKER=deck DJ=off nohup python3 board_server.py > "$DIR/logs/board.log" 2>&1 < /dev/null &
fi

# When run by hand, show the result.
if [ -t 1 ]; then
    sleep 3
    grep -E "deck|FIFO|resmgr" "$DIR/logs/engine.log"
    [ -f "$DIR/logs/board.log" ] && [ -x "$DIR/board/deck" ] && grep -E "^\[(dj|ready|board)\]" "$DIR/logs/board.log" | head -4
    ip=$(ifconfig bcm0 | awk '/inet /{print $2}')
    echo "Audio: http://$ip:8080/    Board view: http://$ip:8081/"
fi
