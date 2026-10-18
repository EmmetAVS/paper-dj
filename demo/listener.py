#!/usr/bin/env python3
"""Acts like a phone: pulls the engine's audio stream and checks it keeps up with real time."""
import socket, sys, time
dur = float(sys.argv[1] if len(sys.argv) > 1 else 10)
port = int(sys.argv[2] if len(sys.argv) > 2 else 8080)
s = socket.create_connection(("127.0.0.1", port))
s.sendall(b"GET /stream HTTP/1.1\r\n\r\n")
_, _, body = s.recv(65536).partition(b"\r\n\r\n")
n, t0 = len(body), time.time()
last, gap = t0, 0.0
while time.time() - t0 < dur:
    d = s.recv(65536)
    if not d:
        break
    now = time.time(); gap = max(gap, now - last); last = now; n += len(d)
el = time.time() - t0
print("  phone stream: %.1f s of audio in %.1f s (%.1f%% of real time), longest gap %.0f ms"
      % (n / 4 / 48000, el, 100 * n / 4 / 48000 / el, gap * 1000))
