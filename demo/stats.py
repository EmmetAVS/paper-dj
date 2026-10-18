#!/usr/bin/env python3
"""One-line summary of the engine's real-time stats."""
import json, sys, urllib.request
port = int(sys.argv[1] if len(sys.argv) > 1 else 8080)
s = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/stats"))
print("  audio: deadline_misses=%d  worst wake-up %.2f ms (budget 5.33)  render %.0f us (max %.0f)  render CPU %.1f%%"
      % (s["deadline_misses"], s["max_late_us"] / 1000, s["render_us"], s["render_us_max"], s["cpu_pct"]["render"]))
