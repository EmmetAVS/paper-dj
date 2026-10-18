#!/usr/bin/env python3
"""End-to-end test of every pad and slider on both decks.

Starts ./dj_engine with the generated test tracks, drives it over UDP exactly
like the vision process will, and checks /stats plus the actual audio stream.

    make && python3 tools/make_test_tracks.py && python3 tests/feature_test.py
"""
import json, math, os, platform, socket, struct, subprocess, sys, time, urllib.request

HTTP, UDP = 8090, 9090
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("  PASS " if ok else "  FAIL ") + name + (f"  ({detail})" if detail else ""))
    if not ok:
        fails += 1


_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def send(cmd, wait=0.08):
    _udp.sendto(cmd.encode(), ("127.0.0.1", UDP))
    time.sleep(wait)


def stats():
    return json.load(urllib.request.urlopen(f"http://127.0.0.1:{HTTP}/stats"))


def deck(k):
    return stats()["decks"]["ab".index(k)]


def capture(sec):
    """Returns (left, right) int lists for `sec` seconds of fresh audio."""
    s = socket.create_connection(("127.0.0.1", HTTP))
    s.sendall(b"GET /stream HTTP/1.1\r\n\r\n")
    data, t0 = b"", time.time()
    while time.time() - t0 < sec + 0.05:
        data += s.recv(65536)
    s.close()
    body = data.split(b"\r\n\r\n", 1)[1][2400 * 4:]  # drop the 50 ms backlog
    n = len(body) // 4
    v = struct.unpack("<%dh" % (2 * n), body[: 4 * n])
    return v[0::2], v[1::2]


def rms(x):
    return math.sqrt(sum(s * s for s in x) / max(1, len(x))) / 32768


def hf(x):
    """High-frequency energy: RMS of the first difference."""
    return rms([x[i + 1] - x[i] for i in range(len(x) - 1)])


def brightness(x):
    """High-frequency share: RMS of the first difference over RMS of the signal."""
    d = [x[i + 1] - x[i] for i in range(len(x) - 1)]
    return rms(d) / max(1e-9, rms(x))


def reset():
    for k in "ab":
        send(f"{k}.pause;{k}.sync 0;{k}.loop 0;{k}.fx 0;{k}.scratch 0;{k}.vol 0.8;{k}.filter 0;"
             f"{k}.low 0;{k}.mid 0;{k}.high 0;{k}.rate 1;{k}.hotcue.clear 1;{k}.hotcue.clear 2;{k}.seek 0", 0)
    send("s1.stop;s2.stop;master 0.9;quantize 1", 0.3)


def test_deck(k, pads):
    other = "b" if k == "a" else "a"
    play, cue, hc1, hc2, loop, sync, fx = pads
    print(f"\nDeck {k.upper()}  (pads {play},{cue},{hc1},{hc2},{loop},{sync},{fx})")
    reset()
    send(f"xfade {0 if k == 'a' else 1}", 0.2)

    # Play / pause
    send(f"pad {play}", 0.5)
    check("play/pause pad starts the deck", deck(k)["playing"] == 1)
    check("deck is audible", rms(capture(0.3)[0]) > 0.02)
    send(f"pad {play}", 0.3)
    check("play/pause pad stops the deck", deck(k)["playing"] == 0)
    check("stopped deck is silent", rms(capture(0.3)[0]) < 0.001)

    # Cue
    send(f"{k}.seek 0;pad {play}", 1.0)
    send(f"pad {cue}", 0.2)
    d = deck(k)
    check("cue while playing jumps back to cue point and stops", d["playing"] == 0 and d["pos"] < 0.05, f"pos {d['pos']:.3f}")

    # Hot cues 1 and 2
    send(f"{k}.seek 4.1", 0.1)
    send(f"pad {hc1}", 0.1)
    h = deck(k)["hotcues"][0]
    beat = 60 / deck(k)["bpm"]
    check("hot cue 1 stores (snapped to beat)", h >= 0 and abs(h / beat - round(h / beat)) < 0.01, f"stored {h:.3f}s")
    send(f"{k}.seek 9.0", 0.1)
    send(f"pad {hc2}", 0.1)
    h2 = deck(k)["hotcues"][1]
    check("hot cue 2 stores", h2 >= 8.5, f"stored {h2:.3f}s")
    send(f"{k}.seek 20", 0.1)
    send(f"pad {hc1}", 0.25)
    d = deck(k)
    check("hot cue 1 jumps and plays", d["playing"] == 1 and abs(d["pos"] - h - 0.25) < 0.15, f"pos {d['pos']:.2f}")
    send(f"pad {hc2}", 0.25)
    d = deck(k)
    check("hot cue 2 jumps", abs(d["pos"] - h2 - 0.25) < 0.15, f"pos {d['pos']:.2f}")

    # Loop
    send(f"pad {loop}", 0.1)
    d = deck(k)
    length_beats = (d["loop_out"] - d["loop_in"]) / beat
    check("loop pad starts a 4-beat loop", d["loop"] == 1 and abs(length_beats - 4) < 0.05, f"{length_beats:.2f} beats")
    time.sleep(4 * beat * 1.6)
    d = deck(k)
    check("playhead stays inside the loop", d["loop_in"] - 0.01 <= d["pos"] <= d["loop_out"] + 0.01,
          f"pos {d['pos']:.2f} in [{d['loop_in']:.2f},{d['loop_out']:.2f}]")
    send(f"pad {loop}", 0.1)
    check("loop pad exits the loop", deck(k)["loop"] == 0)

    # Sync: both decks playing at different tempos
    send(f"{other}.play;{other}.seek 3.3", 0.3)
    send(f"pad {sync}", 2.0)
    d, o = deck(k), deck(other)
    phase_err = (d["phase"] - o["phase"] + 0.5) % 1 - 0.5
    check("sync matches the other deck's tempo", abs(d["eff_bpm"] - o["eff_bpm"]) < 0.3,
          f"{d['eff_bpm']:.2f} vs {o['eff_bpm']:.2f} BPM")
    check("sync locks the beats in phase", abs(phase_err) < 0.03, f"phase error {phase_err:+.3f} beats")
    send(f"pad {sync}", 0.1)
    check("sync pad turns sync off", deck(k)["sync"] == 0)
    send(f"{other}.pause", 0.1)

    # Effects (echo): tail keeps ringing after the deck stops
    send(f"pad {fx}", 0.6)
    check("fx pad turns echo on", deck(k)["fx"] == 1)
    send(f"pad {play}", 0.05)   # stop the deck
    tail = rms(capture(0.3)[0])
    check("echo tail rings after stopping", tail > 0.005, f"rms {tail:.3f}")
    send(f"pad {fx}", 0.1)
    check("fx pad turns echo off", deck(k)["fx"] == 0)

    # Slider 1: volume
    send(f"{k}.seek 0;{k}.play;{k}.vol 0.8", 0.4)
    loud = rms(capture(0.4)[0])
    send(f"{k}.vol 0.2", 0.2)
    quiet = rms(capture(0.4)[0])
    check("volume slider lowers the level", quiet < loud * 0.4, f"{loud:.3f} -> {quiet:.3f}")
    send(f"{k}.vol 0.8", 0.2)

    # Slider 3: EQ / filter knob
    base = capture(0.6)[0]
    send(f"{k}.filter -0.8", 0.3)
    lp = capture(0.6)[0]
    send(f"{k}.filter 0.8", 0.3)
    hp = capture(0.6)[0]
    send(f"{k}.filter 0", 0.2)
    check("filter left = low-pass (removes highs)", hf(lp) < hf(base) * 0.3,
          f"high-freq energy {hf(base):.4f} -> {hf(lp):.4f}")
    check("filter right = high-pass (thinner)", brightness(hp) > brightness(base) * 1.5,
          f"brightness {brightness(base):.3f} -> {brightness(hp):.3f}")
    send(f"{k}.low -40;{k}.mid -40;{k}.high -40", 0.3)
    check("3-band EQ kill", rms(capture(0.3)[0]) < 0.02)
    send(f"{k}.low 0;{k}.mid 0;{k}.high 0", 0.2)

    # Slider 2: scratch (platter)
    send(f"{k}.scratch 1", 0.15)
    p0 = deck(k)["pos"]
    time.sleep(0.3)
    check("hand on platter holds the record", abs(deck(k)["pos"] - p0) < 0.02)
    send(f"{k}.platter 0", 0.05)
    send(f"{k}.platter 180", 0.3)
    p1 = deck(k)["pos"]
    check("rotating forward 180 deg moves +0.9 s", abs(p1 - p0 - 0.9) < 0.05, f"moved {p1 - p0:+.3f}s")
    send(f"{k}.platter 0", 0.3)
    p2 = deck(k)["pos"]
    check("rotating back plays backwards", abs(p2 - p0) < 0.05, f"back to {p2 - p0:+.3f}s")
    send(f"{k}.platter 0;{k}.jog 90", 0.3)
    check("relative jog while scratching", abs(deck(k)["pos"] - p0 - 0.45) < 0.05)
    send(f"{k}.scratch 0", 0.4)
    check("releasing the platter resumes play", deck(k)["pos"] > p0 + 0.6)
    send(f"{k}.pause", 0.1)


def test_track_end():
    print("\nEnd of track")
    reset()
    for k in "ab":
        length = deck(k)["len"]
        send(f"{k}.seek {length - 0.3:.2f};{k}.play", 0.8)
        check(f"deck {k.upper()} stops at the end", deck(k)["playing"] == 0)
        send(f"pad {'2' if k == 'a' else '4'}", 0.1)    # cue while stopped at the end
        check(f"cue at the end is not stored there (deck {k.upper()})", deck(k)["cue"] < length - 0.5)
        send(f"{k}.seek {length - 0.2:.2f}", 0.1)
        send(f"pad {'1' if k == 'a' else '3'}", 0.4)    # play from the end
        d = deck(k)
        check(f"play at the end restarts the song (deck {k.upper()})", d["playing"] == 1 and d["pos"] < 1.0,
              f"pos {d['pos']:.2f}")
        send(f"{k}.pause", 0.05)


def test_samples():
    print("\nSamples")
    reset()
    for pad, idx in ((14, 0), (16, 1)):
        send(f"pad {pad}", 0.0)
        audio = rms(capture(0.3)[0])
        check(f"pad {pad} plays sample {idx + 1}", audio > 0.02, f"rms {audio:.3f}")
        time.sleep(1.2)
        check(f"sample {idx + 1} ends by itself", stats()["samples"][idx]["playing"] == 0)
    send("pad 14", 0.4)
    send("pad 14", 0.02)
    check("sample retriggers from the start", stats()["samples"][0]["playing"] == 1)


def test_both_at_once():
    print("\nBoth songs together")
    reset()
    send("xfade 0.5;a.play;b.play;b.seek 2", 0.5)
    both = rms(capture(0.4)[0])
    send("xfade 0", 0.3)
    only_a = rms(capture(0.4)[0])
    send("xfade 1", 0.3)
    only_b = rms(capture(0.4)[0])
    check("both decks audible at centre", both > 0.03)
    check("crossfader isolates A and B", only_a > 0.02 and only_b > 0.02)
    send("a.fx 1;b.filter -0.5;a.loop 1;b.sync 1", 1.0)
    st = stats()
    a, b = st["decks"]
    check("independent state per deck", a["fx"] and a["loop"] and not b["fx"] and b["sync"] and not a["sync"])
    misses = st["deadline_misses"]
    if platform.system() == "QNX":
        check("no deadline misses during the whole test", misses == 0, f"{misses}")
    else:  # cloud VMs and laptops aren't real-time; only QNX is held to this
        print(f"  INFO deadline misses on {platform.system()}: {misses} (max late {st['max_late_us']:.0f} us); "
              f"render {st['render_us']:.0f} us of a {1e6 * 256 / 48000:.0f} us budget")


def test_priorities():
    print("\nPriorities")
    s = socket.create_connection(("127.0.0.1", HTTP))   # hold a phone stream open
    s.sendall(b"GET /stream HTTP/1.1\r\n\r\n")
    time.sleep(0.3)
    st = stats()
    s.close()
    check("render thread is real-time", st["realtime"] == 1)
    check("phone stream thread is real-time", st["clients"] >= 1 and st["rt_streams"] == st["clients"],
          f"{st['rt_streams']}/{st['clients']} streams at FIFO {st['prio']['stream']}")
    check("ladder: render > control > stream", st["prio"]["render"] > st["prio"]["control"] > st["prio"]["stream"])


def test_empty_deck(exe, t):
    print("\nEmpty deck B")
    proc = subprocess.Popen([exe, "-a", f"{t}/testA_120bpm.wav", "-p", str(HTTP + 1), "-u", str(UDP + 1)],
                            stderr=subprocess.DEVNULL)
    time.sleep(0.6)
    try:
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for c in ("b.play", "pad 7", "pad 8", "pad 11", "pad 12", "pad 15", "b.scratch 1", "b.platter 90",
                  "b.scratch 0", "b.filter -1", "pad 4"):
            u.sendto(c.encode(), ("127.0.0.1", UDP + 1))
        time.sleep(0.3)
        raw = urllib.request.urlopen(f"http://127.0.0.1:{HTTP + 1}/stats").read()
        try:
            json.loads(raw)
            ok = b"nan" not in raw.lower() and b"inf" not in raw.lower()
        except ValueError:
            ok = False
        check("stats stay valid JSON with an empty deck", ok)
        check("engine survives commands to an empty deck", proc.poll() is None)
    finally:
        proc.terminate()
        proc.wait()


def main():
    exe = os.path.join(ROOT, "dj_engine")
    t = os.path.join(ROOT, "tracks")
    proc = subprocess.Popen([exe, "-a", f"{t}/testA_120bpm.wav", "-b", f"{t}/testB_124bpm.wav",
                             "-1", f"{t}/sample1_airhorn.wav", "-2", f"{t}/sample2_zap.wav",
                             "-p", str(HTTP), "-u", str(UDP), "-w", os.path.join(ROOT, "web/index.html")],
                            stderr=subprocess.PIPE, text=True)
    time.sleep(0.8)
    try:
        st = stats()
        print("BPM detected:", [d["bpm"] for d in st["decks"]])
        check("BPM detection deck A (120)", abs(st["decks"][0]["bpm"] - 120) < 0.1)
        check("BPM detection deck B (124)", abs(st["decks"][1]["bpm"] - 124) < 0.1)
        test_deck("a", (1, 2, 5, 6, 9, 10, 13))
        test_deck("b", (3, 4, 7, 8, 11, 12, 15))
        test_track_end()
        test_samples()
        test_both_at_once()
        test_priorities()
    finally:
        proc.terminate()
        proc.wait()
    test_empty_deck(exe, t)
    print(f"\n{'ALL PASSED' if fails == 0 else f'{fails} FAILED'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
