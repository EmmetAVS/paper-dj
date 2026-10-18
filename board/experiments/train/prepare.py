"""Turn labeled recordings into per-button crops.  python3 prepare.py out.npz recordings/<session> [more sessions...]
Each frame -> 16 crops; label 1 for the prompted button on press_/hover_ frames, else 0. The first part of every
prompt is dropped (hand still travelling) and frames right after a change of prompt are skipped."""
import sys, os, glob, numpy as np, cv2
sys.path.insert(0, os.path.dirname(__file__)); from board_crops import find_layout, button_crop, CROP
TRIM = 0.45   # drop the first 45% of each prompt
X, Y, G, K = [], [], [], []   # crops, labels, group id (one per prompt, for honest validation), kind
gid = 0
for sess in sys.argv[2:]:
    fs = sorted(glob.glob(os.path.join(sess, '*.jpg')), key=lambda p: int(os.path.basename(p).split('_')[0]))
    runs, cur = [], []
    for p in fs:
        lab = os.path.basename(p)[:-4].split('_', 1)[1]; t = int(os.path.basename(p).split('_')[0])
        if cur and (cur[-1][1] != lab or t - cur[-1][2] > 700): runs.append(cur); cur = []
        cur.append((p, lab, t))
    if cur: runs.append(cur)
    layout = None; used = 0
    for run in runs:
        gid += 1; keep = run[int(len(run) * TRIM):] if run[0][1] != 'none' else run[int(len(run) * 0.3):]
        lab = run[0][1]; kind = lab.split('_')[0]; want = int(lab.split('_')[1]) if '_' in lab else 0
        for p, _, _ in keep:
            im = cv2.imread(p); L = find_layout(cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)); layout = L or layout
            if layout is None: continue
            for i, q in enumerate(layout):
                X.append(button_crop(im, q)); Y.append(1 if i + 1 == want else 0); G.append(gid); K.append(kind)
            used += 1
    print(f'{sess}: {len(fs)} frames, {len(runs)} prompts, {used} frames kept')
X = np.stack(X); Y = np.array(Y, np.uint8); G = np.array(G); K = np.array(K)
np.savez_compressed(sys.argv[1], X=X, Y=Y, G=G, K=K); print('crops', X.shape, 'positives', int(Y.sum()))
