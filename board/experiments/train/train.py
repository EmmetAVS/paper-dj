"""Train the per-button fingertip model.  python3 train.py crops.npz out_dir [--folds 5] [--epochs 40]
Cleans labels (drops positives where the button looks like it does with hands off), cross-validates by holding out
whole prompts, reports per-frame button accuracy, then trains on everything and exports ONNX (input [N,3,64,64])."""
import sys, os, argparse, json, numpy as np, cv2, torch, torch.nn as nn
ap = argparse.ArgumentParser(); ap.add_argument('data'); ap.add_argument('out'); ap.add_argument('--folds', type=int, default=5)
ap.add_argument('--epochs', type=int, default=40); ap.add_argument('--clean', type=float, default=12.0); ap.add_argument('--seed', type=int, default=0)
a = ap.parse_args(); os.makedirs(a.out, exist_ok=True); rng = np.random.default_rng(a.seed); torch.manual_seed(a.seed)
d = np.load(a.data); X, Y, G, K = d['X'], d['Y'], d['G'], d['K']; n = len(Y) // 16
Xf = X.reshape(n, 16, 64, 64, 3); Yf = Y.reshape(n, 16); Gf = G.reshape(n, 16)[:, 0]; Kf = K.reshape(n, 16)[:, 0]

# --- label cleaning: positive crops indistinguishable from the hands-off view of that button are unreliable ---
gray = Xf.astype(np.float32).mean(-1); none_idx = np.where(Kf == 'none')[0]; valid = np.ones(n, bool)
for f in range(n):
    if Kf[f] == 'none': continue
    b = int(np.argmax(Yf[f])); near = none_idx[np.argsort(np.abs(Gf[none_idx] - Gf[f]))[:12]]
    ref = np.median(gray[near, b], 0); g = gray[f, b] * (ref.mean() / max(gray[f, b].mean(), 1))
    if np.abs(g - ref)[16:48, 16:48].mean() < a.clean: valid[f] = False
print(f'frames {n}, dropped {int((~valid).sum())} with unreliable positive labels, positives kept {int(valid[Kf != "none"].sum())}')

def augment(x):
    """Lighting / colour / camera / orientation changes so the model is not tied to this rig."""
    x = x.astype(np.float32) / 255.
    k = rng.integers(8); x = np.rot90(x, k % 4); x = x[:, ::-1] if k >= 4 else x          # any orientation of hand vs board
    h = 64; M = cv2.getRotationMatrix2D((32, 32), rng.uniform(-12, 12), rng.uniform(0.88, 1.12)); M[:, 2] += rng.uniform(-4, 4, 2)
    x = cv2.warpAffine(np.ascontiguousarray(x), M, (h, h), borderMode=cv2.BORDER_REFLECT)
    x = x * rng.uniform(0.35, 1.6) * rng.uniform(0.8, 1.2, 3)                               # exposure + colour cast
    m = x.mean(); x = (x - m) * rng.uniform(0.6, 1.4) + m                                   # contrast
    x = np.clip(x, 0, 1) ** rng.uniform(0.7, 1.4)                                           # gamma
    if rng.random() < 0.3: x = cv2.GaussianBlur(x, (0, 0), rng.uniform(0.5, 1.5))
    if rng.random() < 0.3:  # uneven light / shadow gradient
        gx, gy = np.meshgrid(np.linspace(-1, 1, 64), np.linspace(-1, 1, 64)); t = rng.uniform(0, 2 * np.pi)
        x = x * (1 + rng.uniform(0, 0.5) * (np.cos(t) * gx + np.sin(t) * gy))[..., None]
    x = x + rng.normal(0, rng.uniform(0, 0.04), x.shape)
    return np.clip(x, 0, 1)

def to_tensor(batch): return torch.tensor(np.stack(batch).transpose(0, 3, 1, 2), dtype=torch.float32)

class Net(nn.Module):
    """~60k params. Input: BGR crop in [0,1], 64x64. Output: logit 'fingertip on the centre button'.
    Per-crop normalisation makes it insensitive to overall exposure."""
    def __init__(s):
        super().__init__()
        c = lambda i, o: nn.Sequential(nn.Conv2d(i, o, 3, padding=1, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True))
        s.f = nn.Sequential(c(3, 16), nn.MaxPool2d(2), c(16, 32), nn.MaxPool2d(2), c(32, 48), nn.MaxPool2d(2), c(48, 64), nn.MaxPool2d(2), c(64, 64))
        s.head = nn.Sequential(nn.Flatten(), nn.Linear(64 * 4 * 4, 64), nn.ReLU(inplace=True), nn.Linear(64, 1))
    def forward(s, x):
        m = x.mean((1, 2, 3), keepdim=True); sd = x.std((1, 2, 3), keepdim=True) + 1e-3
        return s.head(s.f((x - m) / sd)).squeeze(1)

def train(frames):
    idx = [(f, b) for f in frames for b in range(16)]; pos = [i for i in idx if Yf[i]]; neg = [i for i in idx if not Yf[i]]
    net = Net(); opt = torch.optim.AdamW(net.parameters(), 2e-3, weight_decay=1e-3)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 3e-3, total_steps=a.epochs * 40); lossf = nn.BCEWithLogitsLoss()
    for ep in range(a.epochs):
        net.train()
        for it in range(40):   # balanced batches: 32 positives, 96 negatives (hard negatives are crops the arm crosses)
            bi = [pos[j] for j in rng.integers(len(pos), size=32)] + [neg[j] for j in rng.integers(len(neg), size=96)]
            x = to_tensor([augment(Xf[i]) for i in bi]); y = torch.tensor([float(Yf[i]) for i in bi])
            opt.zero_grad(); l = lossf(net(x), y); l.backward(); opt.step(); sched.step()
    return net.eval()

@torch.no_grad()
def predict(net, frames):
    x = torch.tensor(Xf[frames].reshape(-1, 64, 64, 3).astype(np.float32).transpose(0, 3, 1, 2) / 255.)
    return torch.sigmoid(net(x)).numpy().reshape(len(frames), 16)

def frame_decisions(P, th=0.5): return np.where(P.max(1) >= th, P.argmax(1) + 1, 0)

frames_all = np.where(valid)[0]; groups = np.unique(Gf[frames_all]); rng.shuffle(groups)
folds = np.array_split(groups, a.folds); res = []
for k, hold in enumerate(folds):
    tr = frames_all[~np.isin(Gf[frames_all], hold)]; te = frames_all[np.isin(Gf[frames_all], hold)]
    net = train(tr); P = predict(net, te); pred = frame_decisions(P); want = np.where(Kf[te] == 'none', 0, Yf[te].argmax(1) + 1)
    for f, p, w, pr in zip(te, pred, want, P): res.append((Kf[f], int(w), int(p), float(pr.max())))
    print(f'fold {k+1}/{a.folds}: held-out frames {len(te)}, correct {int((pred == want).sum())}', flush=True)
res_k = {}
for kind in ('press', 'hover', 'none'):
    r = [x for x in res if x[0] == kind]
    if r: res_k[kind] = sum(x[1] == x[2] for x in r) / len(r); print(f'  {kind:5s} held-out accuracy: {sum(x[1]==x[2] for x in r)}/{len(r)} = {res_k[kind]:.0%}')
wrong = [x for x in res if x[1] != x[2]]; print('  example mistakes (kind, true, predicted, max prob):', wrong[:10])

net = train(frames_all)
torch.onnx.export(net, torch.zeros(16, 3, 64, 64), os.path.join(a.out, 'fingertip.onnx'), input_names=['crops'], output_names=['logits'],
                  dynamic_axes={'crops': {0: 'n'}, 'logits': {0: 'n'}}, opset_version=17, dynamo=False)
json.dump({'crop': 64, 'context': 2.0, 'channel_order': 'BGR', 'scale': '1/255', 'heldout': res_k, 'frames': int(n), 'clean_threshold': a.clean},
          open(os.path.join(a.out, 'fingertip.json'), 'w'), indent=2)
print('exported', os.path.join(a.out, 'fingertip.onnx'))
