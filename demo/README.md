# QNX priority demo

With `dj_engine` running on the Pi:

```sh
sh ~/paper-dj/demo/stress_demo.sh
```

It starts the vision skeleton, then runs four CPU hogs at priority 35 (one per core): above vision
(32 / 28→8), below audio (50 / 45 / 40).

**Measured on our QNX Pi 5 (QNX 8.0.0), Oct 18 2026:**

| | Before | 4 hogs at priority 35 (run 1) | 4 hogs at priority 35 (run 2) |
|---|---|---|---|
| Audio deadline misses | 0 | **0** | **0** |
| Worst audio wake-up | 0.96 ms | 2.9 ms (budget 5.33 ms) | 2.9 ms (no new worst) |
| Phone stream | real time, max gap 6 ms | **100.6% of real time, max gap 9 ms** | **100.5%, max gap 8 ms** |
| Vision model | 30 fps, 0 dropped, 15 ms latency | 124 frames dropped, latency up to 131 ms | **fully starved: 360 frames dropped, 12 s worst latency** |

In run 2 the hogs held every core for the whole 12 s, so nothing below priority 35 ran at all.
Vision recovered by itself as soon as they stopped.

The full engine test suite (`audio/tests/feature_test.py`) passes 70/70 on the same Pi, with 0 deadline misses.

`pidin` during the run shows the ladder: dj_engine `50f` render, `45f` commands, `40f` phone stream;
vision_skeleton `32f` capture, `28s` model (sporadic); hogs `35r`.

Note: don't use `yes > /dev/null` as the hog on QNX. Every write is a message to the `/dev/null`
server, so `yes` mostly sits blocked and barely loads the CPU. `spin.py` is a real CPU burner that
stops by itself.
