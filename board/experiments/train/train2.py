"""Fine-tune a pretrained MobileNetV3-small as the per-button fingertip model.
    python3 train2.py crops.npz out_dir [--folds 4] [--epochs 12] [--size 96]
Same data/cleaning/validation protocol as train.py; augmentation runs on the GPU (MPS) for speed.
Exported ONNX takes BGR crops in [0,1], [N,3,64,64], and returns logits [N]."""
import os, argparse, json, numpy as np, torch, torch.nn as nn, torch.nn.functional as F, torchvision
ap = argparse.ArgumentParser(); ap.add_argument('data'); ap.add_argument('out'); ap.add_argument('--folds', type=int, default=4)
ap.add_argument('--epochs', type=int, default=12); ap.add_argument('--size', type=int, default=96); ap.add_argument('--clean', type=float, default=12.0)
ap.add_argument('--seed', type=int, default=0); a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
dev = 'mps' if torch.backends.mps.is_available() else 'cpu'; rng = np.random.default_rng(a.seed); torch.manual_seed(a.seed)
d = np.load(a.data); X, Y, G, K = d['X'], d['Y'], d['G'], d['K']; n = len(Y) // 16
Xf = X.reshape(n, 16, 64, 64, 3); Yf = Y.reshape(n, 16); Gf = G.reshape(n, 16)[:, 0]; Kf = K.reshape(n, 16)[:, 0]
gray = Xf.astype(np.float32).mean(-1); none_idx = np.where(Kf == 'none')[0]; valid = np.ones(n, bool)
for f in range(n):
    if Kf[f] == 'none': continue
    b = int(np.argmax(Yf[f])); near = none_idx[np.argsort(np.abs(Gf[none_idx] - Gf[f]))[:12]]
    ref = np.median(gray[near, b], 0); g = gray[f, b] * (ref.mean() / max(gray[f, b].mean(), 1))
    if np.abs(g - ref)[16:48, 16:48].mean() < a.clean: valid[f] = False
print(f'{dev}: frames {n}, dropped {int((~valid).sum())} unreliable, positives {int(valid[Kf != "none"].sum())}', flush=True)
XT = torch.tensor(Xf.reshape(-1, 64, 64, 3).transpose(0, 3, 1, 2).copy(), dtype=torch.uint8)   # all crops, CPU uint8

class Model(nn.Module):
    def __init__(s, size):
        super().__init__(); s.size = size
        m = torchvision.models.mobilenet_v3_small(weights='IMAGENET1K_V1'); s.features = m.features
        s.head = nn.Sequential(nn.AdaptiveAvgPool2d(1), nn.Flatten(), nn.Dropout(0.2), nn.Linear(576, 1))
        s.register_buffer('mean', torch.tensor([0.485, 0.456, 0.406]).view(1, 3, 1, 1)); s.register_buffer('std', torch.tensor([0.229, 0.224, 0.225]).view(1, 3, 1, 1))
    def forward(s, x):                       # x: BGR in [0,1], 64x64
        x = x.flip(1)                        # -> RGB
        x = F.interpolate(x, size=(s.size, s.size), mode='bilinear', align_corners=False)
        return s.head(s.features((x - s.mean) / s.std)).squeeze(1)

def augment(x):
    """x float [B,3,64,64] on device. Orientation, small shift/scale, exposure, colour cast, contrast, gamma, gradient, blur, noise."""
    B = x.shape[0]; k = int(rng.integers(4)); x = torch.rot90(x, k, (2, 3))
    if rng.random() < .5: x = x.flip(3)
    th = torch.zeros(B, 2, 3, device=x.device); sc = torch.empty(B, device=x.device).uniform_(0.88, 1.12); ang = torch.empty(B, device=x.device).uniform_(-.2, .2)
    th[:, 0, 0] = sc * torch.cos(ang); th[:, 0, 1] = -sc * torch.sin(ang); th[:, 1, 0] = sc * torch.sin(ang); th[:, 1, 1] = sc * torch.cos(ang)
    th[:, :, 2] = torch.empty(B, 2, device=x.device).uniform_(-.12, .12)
    x = F.grid_sample(x, F.affine_grid(th, x.shape, align_corners=False), padding_mode='reflection', align_corners=False)
    x = x * torch.empty(B, 1, 1, 1, device=x.device).uniform_(0.35, 1.6) * torch.empty(B, 3, 1, 1, device=x.device).uniform_(0.8, 1.2)
    m = x.mean((1, 2, 3), keepdim=True); x = (x - m) * torch.empty(B, 1, 1, 1, device=x.device).uniform_(0.6, 1.4) + m
    x = x.clamp(1e-4, 1) ** torch.empty(B, 1, 1, 1, device=x.device).uniform_(0.7, 1.4)
    gx = torch.linspace(-1, 1, 64, device=x.device); t = torch.empty(B, 1, 1, 1, device=x.device).uniform_(0, 6.28)
    x = x * (1 + torch.empty(B, 1, 1, 1, device=x.device).uniform_(0, .5) * (torch.cos(t) * gx.view(1, 1, 1, 64) + torch.sin(t) * gx.view(1, 1, 64, 1)))
    if rng.random() < .3: x = F.avg_pool2d(F.pad(x, (1, 1, 1, 1), mode='replicate'), 3, 1)
    x = x + torch.randn_like(x) * float(rng.uniform(0, .04))
    return x.clamp(0, 1)

def train(frames):
    ids = (frames[:, None] * 16 + np.arange(16)[None]).ravel(); lab = Yf.reshape(-1)[ids]; pos = ids[lab == 1]; neg = ids[lab == 0]
    net = Model(a.size).to(dev); opt = torch.optim.AdamW(net.parameters(), 1e-3, weight_decay=1e-4); steps = a.epochs * 40
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 2e-3, total_steps=steps); lossf = nn.BCEWithLogitsLoss()
    net.train()
    for it in range(steps):
        bi = np.concatenate([rng.choice(pos, 32), rng.choice(neg, 96)])
        x = augment(XT[bi].to(dev).float() / 255.); y = torch.tensor(Yf.reshape(-1)[bi], dtype=torch.float32, device=dev)
        opt.zero_grad(); l = lossf(net(x), y); l.backward(); opt.step(); sched.step()
    return net.eval()

@torch.no_grad()
def predict(net, frames):
    ids = (frames[:, None] * 16 + np.arange(16)[None]).ravel(); out = []
    for i in range(0, len(ids), 512): out.append(torch.sigmoid(net(XT[ids[i:i+512]].to(dev).float() / 255.)).cpu().numpy())
    return np.concatenate(out).reshape(len(frames), 16)

frames_all = np.where(valid)[0]; groups = np.unique(Gf[frames_all]); rng.shuffle(groups); folds = np.array_split(groups, a.folds); res = []; allP = []
for k, hold in enumerate(folds):
    tr = frames_all[~np.isin(Gf[frames_all], hold)]; te = frames_all[np.isin(Gf[frames_all], hold)]
    net = train(tr); P = predict(net, te); allP.append((te, P))
    pred = np.where(P.max(1) >= .5, P.argmax(1) + 1, 0); want = np.where(Kf[te] == 'none', 0, Yf[te].argmax(1) + 1)
    res += [(Kf[f], int(w), int(p), float(pr.max())) for f, p, w, pr in zip(te, pred, want, P)]
    print(f'fold {k+1}/{a.folds}: held-out frames {len(te)}, correct {int((pred == want).sum())}', flush=True)
for th in (0.3, 0.5, 0.7):
    line = []
    for kind in ('press', 'hover', 'none'):
        r = [(w, p) for te, P in allP for f, pr in zip(te, P) if Kf[f] == kind for w, p in [((0 if kind == 'none' else int(Yf[f].argmax()) + 1), (int(pr.argmax()) + 1 if pr.max() >= th else 0))]]
        if r: line.append(f'{kind} {sum(w == p for w, p in r)}/{len(r)} = {sum(w == p for w, p in r)/len(r):.0%}')
    print(f'  threshold {th}: ' + ',  '.join(line))
print('  example mistakes (kind, true, predicted, max prob):', [x for x in res if x[1] != x[2]][:10])
net = train(frames_all).cpu()
torch.onnx.export(net, torch.zeros(16, 3, 64, 64), os.path.join(a.out, 'fingertip.onnx'), input_names=['crops'], output_names=['logits'],
                  dynamic_axes={'crops': {0: 'n'}, 'logits': {0: 'n'}}, opset_version=17, dynamo=False)
json.dump({'arch': 'mobilenet_v3_small', 'size': a.size, 'crop': 64, 'input': 'BGR [0,1] 64x64'}, open(os.path.join(a.out, 'fingertip.json'), 'w'), indent=2)
print('exported', os.path.join(a.out, 'fingertip.onnx'), flush=True)
