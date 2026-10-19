# Paper DJ

A full DJ controller you print on paper. A Raspberry Pi 5 running **QNX** watches the sheet with
a Pi Camera, turns your hands into DJ commands, mixes two songs in real time, and streams the
result to your phone. Built for the LA Hacks AI Hackathon 2026 (QNX challenge + Beyond the Digital).

```
paper board ──camera──► board/deck (QNX, OpenCV) ──/dev/paperdj──► audio engine (QNX, SCHED_FIFO) ──Wi-Fi──► phone
```

## Contents

- [Inspiration](#inspiration)
- [What it does](#what-it-does)
- [The board](#the-board)
- [How we built it](#how-we-built-it)
- [Repository layout](#repository-layout)
- [Running it on the Pi](#running-it-on-the-pi)
- [Challenges we ran into](#challenges-we-ran-into)
- [Accomplishments that we're proud of](#accomplishments-that-were-proud-of)
- [What we learned](#what-we-learned)
- [What's next for Paper DJ](#whats-next-for-paper-dj)
- [Built with](#built-with)

## Inspiration

A beginner DJ controller costs from a hundred dollars to upwards of a thousand, which is a lot to
spend just to find out whether you even like DJing. We wanted to get rid of that barrier, turning it
into just a sheet of paper.

QNX provided us with the other part of the idea: DJing is a difficult real-time problem, as a dropped
beat in front of a crowd ruins the set. We wanted to build something where the OS's guarantees are real.

## What it does

Paper DJ turns a printed sheet into a full two-deck DJ controller. A Raspberry Pi 5 running QNX 8
watches the sheet through a Pi Camera.

- **16 printed pads:** play/pause, cue, hot cues, beat loops, sync, an echo effect and two samples.
- **Printed sliders:** volume and filter for each deck.
- **Printed dials:** scratching, with real reverse playback.

The Pi mixes both songs live and streams the result over Wi-Fi to any phone or laptop browser. The
same page also works as a song library: drop an MP3 on it and the Pi converts it, finds its tempo
and beat grid, and lets you load it onto a deck. The other deck keeps playing without issues.

## The board

- **16 pads**: 1–4 play/cue for A and B · 5–8 hot cues 1–2 for A and B · 9–12 loop/sync for A and B · 13 FX A · 14 sample 1 · 15 FX B · 16 sample 2
- **Each hand** (left = deck A, right = deck B): up/down = volume, rotate = scratch, left/right = filter

## How we built it

Everything runs natively on QNX on the Pi. There are three key parts:

### Board tracker (C++, OpenCV from QNX's open-source repo, QNX's camera library)

- Finds the sheet using printed ArUco corner markers, so the camera can look from any angle.
- Finds the controls by shape and tracks the fingertip by skin color, at about 27–30 fps.
- A pad fires after the fingertip rests on it for 250 ms.

### Audio engine (C, written from scratch)

- **DSP:**
  - 3-band EQ and a one-knob DJ filter, built from biquad filters
  - equal-power crossfader
  - beat-synced echo
  - tempo/BPM detection
  - sync that matches tempo and phase
- **Real-time core:** a render thread wakes every 5.33 ms against an absolute clock. It never blocks, allocates memory or takes a lock.

### QNX at the core

- **Priority ladder:** `SCHED_FIFO` priority 50 for audio, 45 for incoming commands, 40 for the phone streams, 32 for camera capture.
- **Hand model:** QNX's `SCHED_SPORADIC` policy gets a CPU budget, then drops in priority so it can never starve the camera.
- **Locks:** shared locks use priority inheritance.
- **Resource manager:** the engine registers `/dev/paperdj`. The tracker sends every pad press and slider move as a `write()`, which QNX delivers as a message straight to the engine. The engine handles it at the sender's priority. `cat /dev/paperdj` shows both decks live.
- **Boot:** the whole system starts from QNX's boot script.

| 50 | 45 | 40 | 32 | 28 → 8 | 10 |
|---|---|---|---|---|---|
| audio render | commands in | phone streams | camera + pads | hand model (sporadic) | everything else |

Verified on our QNX Pi 5: 70/70 engine tests pass, and audio holds 0 deadline misses while CPU hogs at priority 35
starve vision. See [`demo/README.md`](demo/README.md).

## Repository layout

| Folder | What it is | Status |
|---|---|---|
| [`audio/`](audio/) | Two-deck real-time mixing engine + phone player / test board | working, 76/76 tests |
| [`vision/`](vision/) | Priority helpers, engine link library, two-thread pipeline skeleton used for the priority demo | reference; the live tracker is `board/` |
| [`board/`](board/) | Camera tracker for the printed sheets (buttons, sliders, dials) → engine commands, board view page | working |

Start with [`audio/README.md`](audio/README.md), then [`vision/README.md`](vision/README.md).

## Running it on the Pi

```sh
sh ~/paper-dj/start.sh      # (re)starts everything; songs are set at the top of the file
cat /dev/paperdj            # live status (QNX resource manager)
echo "pad 1" > /dev/paperdj # press a pad from the shell
```

To start at boot, the Pi's boot script calls `start.sh` (see [`docs/autostart.md`](docs/autostart.md)).

## Challenges we ran into

- **Hand tracking from overhead is hard.** MediaPipe Hands rarely found a hand when the palm faced away from the camera. A small vision model on ONNX Runtime took about 0.27 s per decision and was no better than a coin flip at telling a touch from a hover. A per-button classifier we trained was too noisy. We ended up with marker-based board detection plus skin-color fingertip tracking, which is fast and reliable.
- **No camera header on the image.** The Pi image doesn't ship the header for QNX's camera API, so we rebuilt the declarations ourselves and checked them against live buffer dumps.
- **Our first stress test didn't stress anything.** Writing to `/dev/null` barely loads the CPU on QNX, because writing to `/dev/null` is a message to a server process rather than a kernel shortcut, so we used a real CPU burner instead.
- **Real songs broke our assumptions.** BPM detection read "Music Sounds Better With You" as 96 instead of 124, so we added tempo hints that lock the exact beat grid. Decks also got stuck at the end of songs, which we fixed.
- **Networking.** Phone hotspots, campus Wi-Fi, and networks that route local traffic over IPv6 all added problems.

## Accomplishments that we're proud of

- **Audio holds under full load.** When we ran CPU hogs at a higher priority than vision, audio missed no deadlines and the phone stream kept pace with real time (100.5%), even while vision dropped frames.
- **Loading songs doesn't touch the audio.** Swapping a 4-minute song onto a deck takes about 1.25 s, with 0 missed deadlines on the deck that's playing.
- **A working, tested system.** 86 automated checks cover every pad, slider and library feature on both decks.
- **It works as a real instrument** running on just a sheet of paper, a Pi, and whatever phone is in your pocket.

## What we learned

- Real-time design is mostly about what the critical thread doesn't do. No locks, no memory allocation and no waiting on the audio path mattered more than any other type of optimization.
- QNX's message-passing design shapes everything. Even writing to a file is a message to a server, which is what made the resource manager natural.
- Priority inheritance and sporadic scheduling aren't textbook trivia, but are key to keeping a heavy workload from taking compute away from the parts that must never pause.

## What's next for Paper DJ

- A learned touch-vs-hover model in the tracker, now that we have a fast pipeline to collect training data.
- Local audio output through QNX's audio stack and a USB sound card for zero Wi-Fi latency.
- Crash recovery: keep deck state in shared memory so a restarted engine picks up mid-song.
- More effects and pad layouts. Since the board is paper, a new layout is just a new print.

## Built with

`c` · `c++` · `make` · `opencv` · `python` · `qnx` · `raspberrypi`
