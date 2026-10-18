"""Score fingertip-to-button detection offline on a labeled recording (made with the page's "Record test set").

    PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 eval_fingertip.py recordings/<session> [--complexity 0|1] [--size 640]

Frames are named <ms>_<label>.jpg with label none | press_N | hover_N. For each frame we locate the 16 buttons,
run MediaPipe Hands, take the index fingertip (landmark 8) and map it to the button that contains it.
Reports: press frames whose fingertip lands on the right button, hover frames (same, since an overhead camera
cannot tell hover from touch), and none frames that wrongly report a button. Also per-frame speed."""
import sys, os, glob, time, argparse, collections
import numpy as np, cv2
from mediapipe.python.solutions import hands as mph

ap = argparse.ArgumentParser(); ap.add_argument('session'); ap.add_argument('--complexity', type=int, default=0)
ap.add_argument('--size', type=int, default=640); ap.add_argument('--conf', type=float, default=0.3); ap.add_argument('--save-misses', action='store_true')
a = ap.parse_args()

det = cv2.aruco.ArucoDetector(cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50), cv2.aruco.DetectorParameters())
def find_layout(gray):
    """Same rule as boarddemo: 16 bright squares (as bright as the paper around them), sorted into a 4x4 grid."""
    _, _, rej = det.detectMarkers(gray); qs = []
    for q in rej:
        q = q.reshape(4, 2); area = cv2.contourArea(q)
        if area < 1500: continue
        m = np.zeros(gray.shape, np.uint8); cv2.fillConvexPoly(m, q.astype(np.int32), 255)
        c = q.mean(0); big = (c + (q - c) * 1.3).astype(np.int32); ring = np.zeros_like(m); cv2.fillConvexPoly(ring, big, 255); ring[m > 0] = 0
        if cv2.mean(gray, m)[0] < 0.6 * cv2.mean(gray, ring)[0]: continue
        qs.append((q, c, area))
    if len(qs) < 16: return None
    med = np.median([x[2] for x in qs]); qs = sorted(qs, key=lambda x: abs(np.log(x[2] / med)))[:16]
    if any(x[2] < med * .6 or x[2] > med * 1.6 for x in qs): return None
    qs.sort(key=lambda x: x[1][1]); rows = [sorted(qs[r*4:r*4+4], key=lambda x: x[1][0]) for r in range(4)]
    return [x[0] for r in rows for x in r]

files = sorted(glob.glob(os.path.join(a.session, '*.jpg')), key=lambda p: int(os.path.basename(p).split('_')[0]))
if not files: sys.exit('no frames in ' + a.session)
layout = None; stats = collections.defaultdict(lambda: [0, 0]); confusion = collections.Counter(); times = []; misses = []
with mph.Hands(static_image_mode=False, max_num_hands=1, model_complexity=a.complexity, min_detection_confidence=a.conf, min_tracking_confidence=0.3) as hands:
    for p in files:
        label = os.path.basename(p)[:-4].split('_', 1)[1]; im = cv2.imread(p); gray = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)
        L = find_layout(gray); layout = L or layout
        if layout is None: continue
        small = cv2.resize(im, (a.size, a.size * im.shape[0] // im.shape[1])); t = time.time()
        r = hands.process(cv2.cvtColor(small, cv2.COLOR_BGR2RGB)); times.append((time.time() - t) * 1000)
        pred = 0
        if r.multi_hand_landmarks:
            lm = r.multi_hand_landmarks[0].landmark[8]; tip = (lm.x * im.shape[1], lm.y * im.shape[0])
            for i, q in enumerate(layout):
                if cv2.pointPolygonTest(q.astype(np.float32), tip, False) >= 0: pred = i + 1; break
        kind, want = (label.split('_')[0], int(label.split('_')[1])) if '_' in label else ('none', 0)
        ok = pred == want; stats[kind][0] += ok; stats[kind][1] += 1; confusion[(kind, want, pred)] += 1
        if not ok: misses.append((p, label, pred))
print(f'{len(files)} frames, {a.size}px, complexity {a.complexity}: median {np.median(times):.0f} ms/frame ({1000/np.median(times):.1f} fps)')
for kind in ('press', 'hover', 'none'):
    if stats[kind][1]: print(f'  {kind:5s}: {stats[kind][0]}/{stats[kind][1]} = {stats[kind][0]/stats[kind][1]:.0%}')
wrong = [(k, v) for k, v in confusion.items() if k[1] != k[2]]
if wrong: print('  most common mistakes (kind, true, predicted 0=none):', sorted(wrong, key=lambda x: -x[1])[:8])
if a.save_misses:
    os.makedirs('misses', exist_ok=True)
    for p, label, pred in misses[:40]: cv2.imwrite(os.path.join('misses', f'{label}_pred{pred}_' + os.path.basename(p)), cv2.imread(p))
