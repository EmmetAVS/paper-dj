# Paper DJ — audio engine

Renders a two-deck DJ mix on the QNX Pi and streams it over Wi-Fi to a phone's browser.
Every pad and slider on the paper board becomes a one-line text command.

```
vision process ──UDP :9000──┐
phone test board ─HTTP /cmd─┤
                            ▼
                  command queue (lock-free on the read side)
                            ▼
        render thread  SCHED_FIFO 50, wakes every 5.33 ms on an absolute clock
        2 decks · hot cues · beat loops · sync · scratch · EQ · filter · echo · 2 samples
                            ▼
                  broadcast ring (~2.7 s)
                            ▼
        HTTP :8080 /stream ──Wi-Fi/hotspot──► phone browser (jitter buffer → speaker / BT headphones)
```

Priorities: render 50, command
receiver 45, phone streams 40 (all FIFO). They sit above the vision process (32 and a sporadic
28→8), so heavy vision work can never starve the audio. The full ladder is in
[`../vision/README.md`](../vision/README.md).

The render thread never blocks, allocates or takes a lock. Commands are parsed on the
control thread (SCHED_FIFO 45) and applied at the start of the next 256-frame block.
A slow or disconnected phone only affects its own stream thread.

## The board

| Pad | Action | Pad | Action |
|---|---|---|---|
| 1 | Deck A play / pause | 9 | Deck A loop (4 beats) on/off |
| 2 | Deck A cue | 10 | Deck A sync on/off |
| 3 | Deck B play / pause | 11 | Deck B loop on/off |
| 4 | Deck B cue | 12 | Deck B sync on/off |
| 5 | Deck A hot cue 1 | 13 | Deck A effect (echo) on/off |
| 6 | Deck A hot cue 2 | 14 | Sample 1 |
| 7 | Deck B hot cue 1 | 15 | Deck B effect (echo) on/off |
| 8 | Deck B hot cue 2 | 16 | Sample 2 |

The vision process just sends `pad <n>`. The mapping lives in one place, `PAD_MAP` in
`src/mixer.c` (mirrored in `web/index.html`), so the board can change without touching vision code.

Each hand controls one deck (left = A, right = B) with three sliders:

| Gesture | Command | Range |
|---|---|---|
| Up / down → volume | `a.vol <v>` | 0 – 1 |
| Rotate → scratch | `a.scratch 1`, then `a.platter <deg>` (absolute, unwrapped), then `a.scratch 0` | 360° = 1.8 s of audio (a 33⅓ rpm record) |
| Left / right → EQ / filter | `a.filter <x>` | −1 (low-pass) … 0 (off) … +1 (high-pass) |

### How each feature behaves

- **Play / pause**: 4 ms fade, so no clicks.
- **Cue** (CDJ-style): while playing, jumps back to the cue point and stops. While stopped, sets the cue point here.
- **Hot cues**: an empty hot cue stores the current position, snapped to the nearest beat. A set hot cue jumps there and plays. Clear with `a.hotcue.clear 1`.
- **Loop**: a 4-beat loop starting on the nearest beat. Press again to exit. Change the length with `a.looplen 1|2|4|8|16`.
- **Sync**: matches this deck's tempo to the other deck and jumps onto its beat. A small (≤ 2%) speed correction then keeps the phase locked. Turning sync off keeps the matched tempo. If both decks are synced, the one synced last follows.
- **Effect**: a half-beat echo, post-fader. Pulling the fader down or stopping the deck leaves the tail ringing. Set the strength with `a.fxamt`.
- **Samples**: one-shots that play over the master, independent of the crossfader. Pressing again retriggers. Volume via `s1.vol` / `s2.vol`.
- **Scratch**: while a hand is on the platter, the record holds still. Rotating moves the playhead forward or backward, with real reverse playback and inertia. Releasing resumes play. Works on a stopped deck too.
- **Filter**: one knob, like a DJM. Left sweeps a resonant low-pass down from 20 kHz to 60 Hz. Right sweeps a high-pass up from 15 Hz to 8 kHz.

BPM and the beat grid are detected automatically when a track loads (about 50 ms for 30 s of audio). If it's wrong for a real song (it read *Music Sounds Better With You* as 96 instead of ~124), give a hint: `-A 124` searches 122–126 and locks the exact tempo **and** the beat positions. `a.bpm 128` only changes the number.

## Build and run on the Pi

```sh
# from your laptop
scp -r paper-dj qnxuser@<pi-ip>:~/

# on the Pi
cd ~/paper-dj/audio
make                                   # uses -lsocket automatically on QNX
python3 tools/make_test_tracks.py      # two 30 s test loops + two samples in tracks/
./dj_engine -a tracks/testA_120bpm.wav -b tracks/testB_124bpm.wav \
            -1 tracks/sample1_airhorn.wav -2 tracks/sample2_zap.wav
```

Startup should print `[render] running SCHED_FIFO priority 50` and a detected BPM per deck. If it
can't get real-time priority, run with `-R 30 -C 25` or as root.

**Phone:** join the same Wi-Fi as the Pi (easiest: the Pi joins the phone's hotspot), open
`http://<pi-ip>:8080/`, and tap **Tap to start audio**. The page doubles as a full test board: all
16 pads, the volume and filter sliders, a scratch platter you drag around, EQ, tempo and loop length.
Pads light up to show state. On iPhone, turn the ringer on the first time if you hear nothing.

**Your own music:** convert it on a laptop, then scp it to the Pi:
`ffmpeg -i song.mp3 -ar 48000 -ac 2 -sample_fmt s16 song.wav`

Options:

- `-a` / `-b`: deck WAVs
- `-1` / `-2`: sample WAVs
- `-A` / `-B`: BPM overrides
- `-p`: HTTP port (default 8080)
- `-u`: UDP port (default 9000)
- `-w`: path to the phone page
- `-R` / `-C` / `-S`: render, control and phone-stream priorities (defaults 50 / 45 / 40)
- `-d`: resource manager path (default `/dev/paperdj`)

## `/dev/paperdj` (QNX resource manager)

The engine also registers the board as a file. Anything on the Pi can drive it with ordinary
file I/O, and `cat` shows the live state:

```sh
echo "pad 1" > /dev/paperdj                    # deck A play
echo "a.vol 0.6; b.filter -0.4" > /dev/paperdj # several commands at once
cat /dev/paperdj                               # status of both decks
```

This is how vision sends its commands (`pdj_link` writes here, and falls back to UDP if the file is missing). There's no extra process: a thread inside `dj_engine` (`src/resmgr.c`) owns the path. Each
`open`/`write`/`read` is a QNX message sent straight to that thread. While handling it, the thread
runs at the caller's priority. A bad command makes the write fail with `EINVAL`. Creating the path
needs root, so `start.sh` runs the engine as root. As a normal user the engine prints a note and
carries on without it. Use `-d <path>` to change the path.

## Full command reference

Plain text, several per message separated by `;` or newlines. write it to `/dev/paperdj` (this is what vision uses), send it over UDP to port 9000, or `GET /cmd?c=<url-encoded>`
for testing. Replace `a.` with `b.` for deck B.

| Command | Effect |
|---|---|
| `pad <1-16>` | press a board pad (see table above) |
| `a.play` `a.pause` `a.toggle` | transport |
| `a.cue` `a.setcue` | CDJ cue / set cue here |
| `a.hotcue <1\|2>` `a.hotcue.clear <1\|2>` | hot cues |
| `a.loop [0\|1]` `a.looplen <beats>` | beat loop (no value = toggle) |
| `a.sync [0\|1]` | tempo + phase sync to the other deck |
| `a.fx [0\|1]` `a.fxamt <0-1>` | echo |
| `a.scratch <0\|1>` `a.platter <deg>` `a.jog <deg>` | scratch: hand on/off, absolute angle, relative turn (jog when not scratching = pitch bend) |
| `a.vol <0-1>` | channel fader |
| `a.filter <-1..1>` | one-knob filter |
| `a.low` `a.mid` `a.high <-40..6 dB>` | 3-band EQ (−40 ≈ kill) |
| `a.rate <0.5-1.5>` `a.nudge <-0.5..0.5>` | tempo / temporary speed offset |
| `a.seek <s>` `a.bpm <bpm>` | jump / set tempo |
| `s1.play` `s1.stop` `s1.vol <0-1>` | samples (`s2.` for sample 2) |
| `xfade <0-1>` `master <0-1>` `quantize [0\|1]` | mixer / snap cues and loops to beats |

All continuous values are smoothed by 10–30 ms, so vision can send raw, jittery values.

```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"pad 5", ("127.0.0.1", 9000))
s.sendto(b"a.vol 0.72;b.filter -0.4", ("127.0.0.1", 9000))
```

## Tests

```sh
make && python3 tools/make_test_tracks.py && python3 tests/feature_test.py
```

The test drives every pad and slider on **both decks** over UDP, exactly as vision will. It checks
the engine state and the real audio stream, and it also runs both songs together. Current result:
all 70 checks pass (including real-time priority of the phone stream and an empty-deck case), including under thread- and address-sanitizer builds.

## Stats (the QNX evidence)

`GET /stats` (also shown live on the phone page) includes:

- `realtime`: whether the render thread got SCHED_FIFO
- `rt_streams` / `clients`: how many phone stream threads got SCHED_FIFO (should be equal)
- `prio`: the configured render / control / stream priorities
- `cpu_pct`: CPU use of the render thread, the control thread and the whole engine (% of one core)
- `deadline_misses`: blocks where the render thread woke more than one block (5.33 ms) late. On QNX this should stay **0**.
- `max_late_us`, `render_us`, `render_us_max`: wake-up lateness and per-block render time
- `cmd_lat_us`, `cmd_lat_us_max`: time from a command arriving to it being applied to the audio
- `via`: commands accepted from `/dev/paperdj` (`dev`), UDP (`udp`) and the web page (`http`)
- per-deck state: BPM, beat phase, hot cues, loop, sync, fx, scratching, and every slider

**Stress demo:** while a mix plays, load the CPU (`sh ../demo/stress_demo.sh   # see demo/README.md`, then
the hogs stop by themselves) and show `deadline_misses` holding at 0. `pidin -p dj_engine` shows the thread
priorities (`50f` = priority 50, FIFO).

Measured on a 2-vCPU Linux cloud VM with every feature on at once: about 20–70 µs per 5.33 ms
block (≈1% of a core). The VM itself isn't real-time, so it shows occasional late wake-ups. That's
exactly the contrast to demonstrate on QNX.

## Next steps

- Keep deck state in shared memory so a restarted engine resumes where it was
- Optional local output through `io-audio` (USB audio adapter) for zero Wi-Fi latency
