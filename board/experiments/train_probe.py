"""Fit a linear readout on Laya's decision features for this board (touch vs hover).

Collect labeled frames first (Laya runs on the fingertip button and saves its features):
    ./boarddemo --collect touch  data     # press buttons for ~40 s, all over the board
    ./boarddemo --collect hover  data     # hover / rest fingers just above buttons for ~40 s
Then:
    python3 train_probe.py data probe.txt
and run with:  ./boarddemo --probe probe.txt
Validation holds out contiguous time blocks (neighbouring frames are near-duplicates)."""
import sys, glob, os, numpy as np
d = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else 'probe.txt'
rows = []
for p in sorted(glob.glob(os.path.join(d, '*.feat'))):
    name = os.path.basename(p); label = name.split('_')[0]
    if label not in ('touch', 'hover'): continue
    ts = int(name.split('_')[1]); rows.append((label == 'touch', ts, np.fromfile(p, dtype='<f4')))
if not rows: sys.exit('no touch_/hover_ .feat files in ' + d)
y = np.array([r[0] for r in rows], float); t = np.array([r[1] for r in rows]); X = np.stack([r[2] for r in rows]).astype(np.float64)
print(f'{len(y)} samples: {int(y.sum())} touch, {int((1-y).sum())} hover, {X.shape[1]} features')
if y.sum() < 5 or (1 - y).sum() < 5: sys.exit('need at least 5 of each label')

def fit(X, y, lam):
    mu, sd = X.mean(0), X.std(0) + 1e-6; Z = (X - mu) / sd; w = np.zeros(Z.shape[1]); b = 0.0
    pos = y.mean(); cw = np.where(y > 0, 0.5 / pos, 0.5 / (1 - pos))   # balance classes
    for it in range(800):                                                 # plain gradient descent, L2
        p = 1 / (1 + np.exp(-(Z @ w + b))); g = cw * (p - y)
        w -= 0.1 * (Z.T @ g / len(y) + lam * w); b -= 0.1 * g.mean()
    return w, b, mu, sd
def predict(m, X): w, b, mu, sd = m; return 1 / (1 + np.exp(-(((X - mu) / sd) @ w + b)))

# blocked cross-validation over time within each label (5 folds)
folds = np.zeros(len(y), int)
for lab in (0, 1):
    idx = np.where(y == lab)[0]; idx = idx[np.argsort(t[idx])]
    for k, chunk in enumerate(np.array_split(idx, 5)): folds[chunk] = k
best = None
for lam in (1e-3, 1e-2, 1e-1, 1.0):
    acc = []; 
    for k in range(5):
        tr, te = folds != k, folds == k
        if len(set(y[tr])) < 2: continue
        acc.append(((predict(fit(X[tr], y[tr], lam), X[te]) >= .5) == y[te]).mean())
    a = float(np.mean(acc)); print(f'lambda {lam:g}: held-out-block accuracy {a:.3f}')
    if best is None or a > best[0]: best = (a, lam)
m = fit(X, y, best[1]); w, b, mu, sd = m
train_acc = ((predict(m, X) >= .5) == y).mean()
with open(out, 'w') as f:
    f.write(f'{len(w)}\n{b:.8g}\n' + ' '.join(f'{v:.8g}' for v in w) + '\n' + ' '.join(f'{v:.8g}' for v in mu) + '\n' + ' '.join(f'{v:.8g}' for v in sd) + '\n')
print(f'chose lambda {best[1]:g} (held-out {best[0]:.3f}, train {train_acc:.3f}); wrote {out}')
