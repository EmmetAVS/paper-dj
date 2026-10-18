"""Offline score of skin-colour fingertip detection on a labeled recording (mirrors boarddemo --mask skin).
python3 eval_skin.py recordings/<session> [--th 8] [--grow 1.6]
Uses the settled part of each prompt (last 55%) to avoid the 'hand still travelling' label lag."""
import sys, os, glob, argparse, collections, numpy as np, cv2
sys.path.insert(0, os.path.dirname(__file__)); from board_crops import find_layout, button_crop
ap = argparse.ArgumentParser(); ap.add_argument('session'); ap.add_argument('--th', type=float, default=8); ap.add_argument('--grow', type=float, default=1.6)
ap.add_argument('--ymin', type=float, default=40); ap.add_argument('--save', action='store_true'); a = ap.parse_args()
K5 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))

def skin_mask(im_q, hull_q):
    c = cv2.cvtColor(im_q, cv2.COLOR_BGR2YCrCb).astype(np.float32); d = c[..., 1] - c[..., 2]
    region = np.zeros(d.shape, np.uint8); cv2.fillConvexPoly(region, hull_q.astype(np.int32), 255)
    d -= np.median(d[region > 0])                                    # paper's own tint = 0
    m = ((d > a.th) & (c[..., 0] > a.ymin) & (region > 0)).astype(np.uint8) * 255
    return cv2.morphologyEx(m, cv2.MORPH_OPEN, K5)

def fingertip(mask, quads_q, grid_hull, min_area=12):
    """Skin patch over the paper = the lit part of the finger. Tip = the end of the patch's long axis that points
    toward the centre of the button grid (fingers reach in from outside), stepped back a little onto the pad."""
    n, lab, st, cen = cv2.connectedComponentsWithStats(mask)
    if n < 2: return None
    best = 1 + int(np.argmax(st[1:, 4]))
    if st[best, 4] < min_area: return None
    ys, xs = np.nonzero(lab == best); P = np.stack([xs, ys], 1).astype(np.float32); m = P.mean(0)
    _, _, vt = np.linalg.svd(P - m, full_matrices=False); ax = vt[0]
    gc = grid_hull.mean(0); proj = (P - m) @ ax
    if (gc - m) @ ax < 0: ax, proj = -ax, -proj                       # orient the axis toward the grid centre
    t = P[int(np.argmax(proj))] - ax * 2.0
    for k, q in enumerate(quads_q):
        if cv2.pointPolygonTest(q.astype(np.float32), (float(t[0]), float(t[1])), False) >= 0: return k + 1
    return 0

fs = sorted(glob.glob(os.path.join(a.session, '*.jpg')), key=lambda p: int(os.path.basename(p).split('_')[0]))
runs, cur = [], []
for p in fs:
    lab = os.path.basename(p)[:-4].split('_', 1)[1]; t = int(os.path.basename(p).split('_')[0])
    if cur and (cur[-1][1] != lab or t - cur[-1][2] > 700): runs.append(cur); cur = []
    cur.append((p, lab, t))
runs.append(cur)
res = collections.Counter(); per = collections.defaultdict(lambda: [0, 0]); layout = None; saved = 0
empty = [collections.deque(maxlen=8) for _ in range(16)]; skipped = 0
for run in runs:
    for p, lab, _ in run[int(len(run) * 0.45):]:
        im = cv2.imread(p); L = find_layout(cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)); layout = L or layout
        if layout is None: continue
        imq = cv2.resize(im, (im.shape[1] // 2, im.shape[0] // 2), interpolation=cv2.INTER_AREA)   # quarter of full sensor res
        qq = [q / 2 for q in layout]; pts = np.concatenate(qq); hull = cv2.convexHull(pts).reshape(-1, 2); c = hull.mean(0)
        region = c + (hull - c) * a.grow; grid = c + (hull - c) * 1.3
        m = skin_mask(imq, region); pred = fingertip(m, qq, grid) or 0
        kind = lab.split('_')[0]; want = int(lab.split('_')[1]) if '_' in lab else 0
        crops = [cv2.cvtColor(button_crop(im, q), cv2.COLOR_BGR2GRAY).astype(np.float32) for q in layout]
        if kind == 'none':
            for b in range(16): empty[b].append(crops[b])
        elif empty[want - 1]:
            ref = np.median(np.stack(empty[want - 1]), 0); g = crops[want - 1] * (ref.mean() / max(crops[want - 1].mean(), 1))
            if np.abs(g - ref)[16:48, 16:48].mean() < 12: skipped += 1; continue      # nothing at the target: bad label
        else: skipped += 1; continue
        per[kind][0] += pred == want; per[kind][1] += 1; res[(kind, want, pred)] += 1
        if a.save and pred != want and saved < 12:
            v = imq.copy(); v[m > 0] = (0, 140, 255); cv2.imwrite(f'skinmiss_{saved}_{lab}_pred{pred}.png', v); saved += 1
print(f'(skipped {skipped} press/hover frames with nothing on the target button)')
print(f'th {a.th} grow {a.grow} ymin {a.ymin}: ' + ',  '.join(f'{k} {v[0]}/{v[1]} = {v[0]/max(1,v[1]):.0%}' for k, v in per.items()))
print('  top mistakes (kind,true,pred):', [x for x in sorted(res.items(), key=lambda x: -x[1]) if x[0][1] != x[0][2]][:8])
