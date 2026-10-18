#!/bin/sh
# Kept for muscle memory: everything now starts from the top-level start.sh
# (engine as root with /dev/paperdj, then this tracker and the board pages).
#   board/start_dj.sh [deckA.wav deckB.wav]      Stop: board/start_dj.sh stop
exec sh "$(cd "$(dirname "$0")" && pwd)/../start.sh" "$@"
