#!/bin/bash
# Paper DJ on the Pi: audio engine + paper board tracker, connected.
#   audio + full deck page:  http://qnxpi.local:8082   (relay to dj_engine :8080; works over IPv6 phone hotspots)
#   board view:              http://qnxpi.local:8081
# Usage: board/start_dj.sh [deckA.wav deckB.wav]      Stop: board/start_dj.sh stop
TD=$(cd "$(dirname "$0")" && pwd); DJ=${DJ_DIR:-$TD/../audio}
stop() {
  for p in $(pidin arg 2>/dev/null | grep -E "board_server.py|dj_engine" | grep -v grep | awk '{print $1}'); do kill $p 2>/dev/null; done
  slay -f deck 2>/dev/null; slay -f boarddemo 2>/dev/null; sleep 1
}
stop; [ "$1" = stop ] && { echo stopped; exit 0; }
A=${1:-$DJ/tracks/testA_120bpm.wav}; B=${2:-$DJ/tracks/testB_124bpm.wav}
cd "$DJ" && nohup ./dj_engine -a "$A" -b "$B" -1 tracks/sample1_airhorn.wav -2 tracks/sample2_zap.wav > "$TD/dj_engine.log" 2>&1 &
sleep 2; grep -E "render|BPM|listen|error" "$TD/dj_engine.log" | head -6
cd "$TD" && slay -f camera_example3_viewfinder 2>/dev/null
TRACKER=deck DJ=off nohup python3 board_server.py > server.log 2>&1 &   # deck sends engine commands itself
sleep 1; echo "audio: http://qnxpi.local:8082    board: http://qnxpi.local:8081"
