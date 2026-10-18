# Paper DJ

A full DJ controller you print on paper. A Raspberry Pi 5 running **QNX** watches the sheet with
a Pi Camera, turns your hands into DJ commands, mixes two songs in real time, and streams the
result to your phone. Built for the LA Hacks AI Hackathon 2026 (QNX challenge + Beyond the Digital).

```
paper board ──camera──► vision (QNX, ONNX hand tracking) ──UDP──► audio engine (QNX, SCHED_FIFO) ──Wi-Fi──► phone
```

| Folder | What it is | Status |
|---|---|---|
| [`audio/`](audio/) | Two-deck real-time mixing engine + phone player / test board | working, 70/70 tests |
| [`vision/`](vision/) | Priority helpers, engine link, two-thread pipeline skeleton (camera, pad check and hand model still fake) | skeleton running |

## The board

- **16 pads**: 1–4 play/cue for A and B · 5–8 hot cues 1–2 for A and B · 9–12 loop/sync for A and B · 13 FX A · 14 sample 1 · 15 FX B · 16 sample 2
- **Each hand** (left = deck A, right = deck B): up/down = volume, rotate = scratch, left/right = filter

## Priorities (QNX)

| 50 | 45 | 40 | 32 | 28 → 8 | 10 |
|---|---|---|---|---|---|
| audio render | commands in | phone streams | camera + pads | hand model (sporadic) | everything else |

Start with [`audio/README.md`](audio/README.md), then [`vision/README.md`](vision/README.md).
