import sys, time, glob, numpy as np, cv2
from mediapipe.python.solutions import hands as mph
imgs = [cv2.imread(p) for p in sys.argv[1:]]
for complexity in (0, 1):
    with mph.Hands(static_image_mode=True, max_num_hands=1, model_complexity=complexity, min_detection_confidence=0.3) as h:
        for p, im in zip(sys.argv[1:], imgs):
            for size in (640,):
                small = cv2.resize(im, (size, size * im.shape[0] // im.shape[1]))
                rgb = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)
                h.process(rgb); t = time.time(); n = 5
                for _ in range(n): r = h.process(rgb)
                ms = (time.time() - t) / n * 1000
                tip = None
                if r.multi_hand_landmarks:
                    lm = r.multi_hand_landmarks[0].landmark[8]; tip = (round(lm.x * im.shape[1]), round(lm.y * im.shape[0]))
                print(f"complexity {complexity} {p.split('/')[-1]} {size}px: {ms:.0f} ms/frame (static mode, detector every frame), index tip={tip}", flush=True)
    with mph.Hands(static_image_mode=False, max_num_hands=1, model_complexity=complexity, min_detection_confidence=0.3) as h:
        im = imgs[-1]; small = cv2.resize(im, (640, 640 * im.shape[0] // im.shape[1])); rgb = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)
        h.process(rgb); t = time.time(); n = 10
        for _ in range(n): r = h.process(rgb)
        print(f"complexity {complexity} tracking mode: {(time.time()-t)/n*1000:.0f} ms/frame, found={bool(r.multi_hand_landmarks)}", flush=True)
