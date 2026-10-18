# Paper DJ — vision

C helpers and a runnable two-thread skeleton for the camera side. The priorities and the link
to the audio engine are already wired up. Plug the real camera, pad detector and hand model
into the three functions marked `FAKE` in `src/vision_skeleton.c`.

## Priority ladder (whole system)

Higher numbers always win on QNX. Everything stays ≤ 63, the default limit for non-root users,
so nothing needs root.

| Priority | Thread | Process | Why |
|---|---|---|---|
| 50 FIFO | audio render | dj_engine | a late block is a click everyone hears |
| 45 FIFO | command receiver | dj_engine | commands land in the next 5.3 ms block |
| 40 FIFO | phone audio streams | dj_engine | delivering audio outranks seeing |
| **32 FIFO** | **camera capture + pad check** | vision | cheap, every frame; pad presses go out from here |
| **28 → 8 sporadic** | **hand-tracking model** | vision | at 28 for up to 20 ms of every 33 ms frame, then drops to 8 |
| 10 | shell, logging, web page | — | nobody hears these |

**Why sporadic for the model:** the model is the one thing that can eat a whole core. With
`SCHED_SPORADIC` it runs fast (priority 28, above all normal work) until it has used its budget for
the frame, then QNX automatically drops it to priority 8 so it can't starve anything else. It
keeps running at 8 if the CPU is free. Nothing in our code has to police it; the kernel does.

**Why a latest-frame mailbox:** if the model falls behind, it skips to the newest frame instead
of working through a backlog. Latency stays bounded and old hand positions never get sent.

## The skeleton

```
camera ──► [capture, FIFO 32] ──pad check every frame──► pdj_pad()        (sent immediately)
                  │
            latest-frame mailbox (priority-inheritance mutex)
                  ▼
           [model, SPORADIC 28→8]  hand landmarks ──► pdj_set("a.vol"), pdj_set("b.filter"),
                                                      pdj_scratch_begin/angle/end
                                                      (coalesced, ≤ 60 packets/s)
```

```sh
cd ~/paper-dj/vision
make
../audio/dj_engine -a ../audio/tracks/testA_120bpm.wav -b ../audio/tracks/testB_124bpm.wav \
                   -1 ../audio/tracks/sample1_airhorn.wav -2 ../audio/tracks/sample2_zap.wav &
./vision_skeleton             # -m <ms> fake model cost, -b <ms> budget, -f <fps>, -c pin to CPUs 0-2
```

With the fake input it plays a scripted routine every 8 s: play A, play B, sync B, store hot cue,
echo on/off, air horn, jump to hot cue. Meanwhile the left hand rides deck A's volume and the right
hand sweeps deck B's filter and does a 1 s scratch. Once a second it prints capture/model fps,
dropped frames, frame-to-command latency, packets sent and per-thread CPU.

Measured on a Linux VM with a 15 ms fake model:

- 30 fps capture and model, 0 dropped frames, about 16 ms frame → command
- 438 of 990 slider updates actually sent, because tiny changes are skipped
- the engine applied all 450 commands with 0 dropped and 0 deadline misses

Overloaded (`-m 45`, more than one frame of work), capture and pad presses still ran at 30 fps
while the model shed frames (22 fps).

`-c` pins the vision threads to CPUs 0–2, so CPU 3 is left for the audio engine. Try it if `pidin`
shows the engine competing with vision.

## Library API (`include/`)

`pdj_rt.h`: real-time setup for the calling thread.

| Function | Use |
|---|---|
| `pdj_rt_fifo(prio, who)` | capture thread at `PDJ_PRIO_CAPTURE` |
| `pdj_rt_sporadic(hi, lo, budget_ms, period_ms, who)` | model thread at `PDJ_PRIO_MODEL_HI`/`_LO` |
| `pdj_rt_pin(mask, who)` | QNX runmask (Linux affinity) |
| `pdj_rt_lock_memory()` | no page-fault stalls |
| `pdj_rt_describe(who)` | print the thread's actual policy and priority |

`pdj_link.h`: commands to the engine.

| Function | Tier |
|---|---|
| `pdj_pad(l, n)` | immediate |
| `pdj_send_now(l, "a.looplen 8")` | immediate (any engine command) |
| `pdj_scratch_begin(l, 'a')` / `pdj_scratch_end(l, 'a')` | immediate, ordered |
| `pdj_scratch_angle(l, 'a', deg)` | coalesced; only the change since touch-down matters (+360° = +1.8 s) |
| `pdj_set(l, "a.vol", v)`, `pdj_set(l, "b.filter", x)` | coalesced, latest value wins |
| `pdj_tick(l)` | call once per processed frame; sends at most every 16 ms |
| `pdj_unwrap_deg(...)` | turns `atan2` angles into a continuous angle for scratching |
| `pdj_pads_update(&pads, covered, l)` | debouncer: per-frame "pad covered?" → clean single presses |

Before any immediate command is sent, pending slider values are flushed, so the engine always sees
events in order. For example, the last platter angle always arrives before `scratch 0`.

## Checking it on the Pi

```sh
pidin -p dj_engine        # threads at 50f, 45f, 40f (one per listening phone)
pidin -p vision_skeleton  # capture at 32f; model shows sporadic at 28 (or 8 while over budget)
```

**Judge demo:** with a mix playing and vision running, start a CPU hog *between* vision and audio:

```sh
on -p 35 yes > /dev/null &
```

Vision slows down, but the engine's `deadline_misses` stays at 0 and the phone keeps playing
cleanly. Then `slay yes`.

Not yet verified on QNX: everything here builds and runs on Linux, but Linux has no sporadic
scheduling, so the skeleton falls back to normal priority there and says so. The QNX code path is
written against the QNX 8.0 `sched_param` docs. Run `vision_skeleton` on the Pi first thing and
check the `[model] SCHED_SPORADIC 28 -> 8 …` line.
