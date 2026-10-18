#!/usr/bin/env python3
"""Pure CPU burner that stops by itself after N seconds (so it can't lock up the Pi).
On QNX `yes > /dev/null` is NOT a good hog: every write is a message to the
/dev/null server, so it spends most of its time blocked."""
import sys, time
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 10)
while time.time() < end:
    pass
