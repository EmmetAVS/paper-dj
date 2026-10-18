#!/bin/sh
# The QNX priority demo. Run on the Pi with dj_engine already running (port 8080).
#   sh demo/stress_demo.sh
# Starts the vision skeleton, then pins every core with CPU hogs at priority 35:
# above vision (32 / 28), below audio (50 / 45 / 40). Vision gets squeezed; audio doesn't.
D=$(cd "$(dirname "$0")" && pwd)
V=$D/../vision
python3 - <<'P'
import socket; u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.sendto(b"a.seek 0;b.seek 0;a.play;b.play;b.sync 1;a.fx 1;xfade 0.5", ("127.0.0.1", 9000))
P
slay -f vision_skeleton >/dev/null 2>&1
"$V/vision_skeleton" -t 40 > /tmp/pdj_vision.log 2>&1 < /dev/null &
sleep 4
echo "=== BEFORE ==="
python3 "$D/stats.py"; tail -1 /tmp/pdj_vision.log
echo "=== 4 CPU hogs at priority 35 (one per core) for 12 s ==="
for i in 1 2 3 4; do on -p 35 python3 "$D/spin.py" 12 & done
sleep 1
python3 "$D/listener.py" 9
tail -1 /tmp/pdj_vision.log
python3 "$D/stats.py"
sleep 4
echo "=== AFTER ==="
tail -1 /tmp/pdj_vision.log
slay -f vision_skeleton >/dev/null 2>&1
