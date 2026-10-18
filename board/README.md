# Paper DJ — board tracker (camera → dj_engine)

Turns the printed sheets into DJ commands on the QNX Pi 5 with Camera Module 3, at ~27–30 fps.
No ML model in the live loop: OpenCV (from `repo.oss.qnx.com`) plus QNX's camera library.

```
Camera Module 3 ──libcamapi NV12 callbacks──► deck ──UDP :9000──► dj_engine (audio/)
                                               │
                                               └─ JSON events ──► board_server.py ──► http://qnxpi.local:8081 (board view)
                                                                   └─ relay :8082 ──► dj_engine page (audio + full deck)
```

## What the sheets do

| Control | Engine command |
|---|---|
| Button N (fingertip rests 250 ms) | `pad N` (mapping lives in `audio/src/mixer.c`) |
| Sliders, in printed order | `a.vol` · `a.filter` · `b.filter` · `b.vol` (filter: middle = off) |
| Dials (left = A, right = B) | `a.scratch 1`, `a.platter <deg>` while circling, `a.scratch 0` on release |

## How `deck` works

- **Finds the controls by shape**, refreshed ~3×/s: the 16 buttons are bright squares (ArUco candidate quads), sliders are
  tall rectangles, dials are circles. Missing buttons are filled in from a fitted grid; sizes adapt to camera height.
- **Sheet orientation from the ArUco corner markers** (DICT_4X4_50): numbering, slider direction and dial order follow
  the sheet, so the camera can look from any angle. Fingertips point "up the sheet" (you reach in from the bottom edge).
- **Fingertip by skin colour, no reference image**: Cr−Cb against a local median of the paper's tint (cancels uneven
  lighting), brightness floor, only on the sheets. Of the skin patches, the one lying most on controls is the finger;
  the fingertip is the end of its long axis pointing up the sheet.
- Buttons need the fingertip inside them (gaps between buttons do nothing), 250 ms dwell, 300 ms release grace.

## Run on the Pi

```sh
cd ~/paper-dj/audio && make && python3 tools/make_test_tracks.py   # engine + test tracks (see audio/README.md)
cd ~/paper-dj/board && make                                         # needs: apk add opencv-dev libopencv_aruco
./start_dj.sh                     # engine + tracker + pages;  ./start_dj.sh stop
```

Open **http://qnxpi.local:8082** for audio (tap "Tap to start audio") and **http://qnxpi.local:8081** for the board view.
Keep hands off the sheets for ~2 s at start. `touch ~/paper-dj/board/debug.req` writes `debug.png` (detected controls,
skin mask, fingertip). `make snap && ./snap out.ppm` grabs one frame (stop the tracker first).

`camera/camera_api.h` is a minimal declaration of QNX's `libcamapi` (the SDP header isn't on the Pi image), checked
against the installed `camera_example*` binaries and a live buffer dump.

## Setup tips

- Fill most of the frame with the sheets (lower camera = bigger fingertips, more markers decode).
- Tape the sheets flat; a lifted seam shows up as a thin false skin line (ignored, but best avoided).
- Reach around the dials, not across them: resting on a dial starts a scratch (which holds the record).

## Experiments (not in the live loop) — `experiments/`

| What | Result |
|---|---|
| `boarddemo.cpp`: background-difference tracker with Laya touch check, pinch faders, labeled recording mode | superseded by `deck` (reference image broke when the paper moved or light changed) |
| Laya (`thaitea/laya-vision`) on ONNX Runtime 1.24.4 (QNX package), `laya/export_board.py` | 128 px INT8 ≈ 0.27 s/decision at 2.4 GHz; touch-vs-hover zero-shot ≈ chance on this board |
| MediaPipe Hands (QNX port), `mp_bench.py`, `eval_fingertip.py` | ~10 fps but rarely finds the hand from overhead (palm not visible) |
| Per-button fingertip CNN (`train/`), trained on a 2-minute guided recording | 29–75 % held-out; recording labels too noisy, dropped |
| Skin-colour scorer (`train/eval_skin.py`) | basis of the current `deck` approach |

Pi notes: CPU runs at 1.5 GHz unless `force_turbo=1` is set in `/boot/config.txt` (QNX has no cpufreq driver);
MediaPipe's Python needs `PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python` on QNX.
