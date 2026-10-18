"""Shared geometry for the per-button fingertip model (used by training and mirrored in boarddemo.cpp).
find_layout(): the 16 printed buttons, row-major. button_crop(): a context crop around one button, warped so the
button is axis-aligned and centred, CROP x CROP pixels, button occupying the middle 1/CONTEXT of the crop."""
import numpy as np, cv2
CROP = 64        # crop side in pixels
CONTEXT = 2.0    # crop covers CONTEXT x the button size (finger + approach direction visible)
_det = cv2.aruco.ArucoDetector(cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50), cv2.aruco.DetectorParameters())

def find_layout(gray):
    _, _, rej = _det.detectMarkers(gray); qs = []
    for q in rej:
        q = q.reshape(4, 2).astype(np.float32); area = cv2.contourArea(q)
        if area < 1500: continue
        m = np.zeros(gray.shape, np.uint8); cv2.fillConvexPoly(m, q.astype(np.int32), 255)
        c = q.mean(0); big = (c + (q - c) * 1.3).astype(np.int32); ring = np.zeros_like(m); cv2.fillConvexPoly(ring, big, 255); ring[m > 0] = 0
        if cv2.mean(gray, m)[0] < 0.6 * cv2.mean(gray, ring)[0]: continue
        qs.append((q, c, area))
    if len(qs) < 16: return None
    med = np.median([x[2] for x in qs]); qs = sorted(qs, key=lambda x: abs(np.log(x[2] / med)))[:16]
    if any(x[2] < med * .6 or x[2] > med * 1.6 for x in qs): return None
    qs.sort(key=lambda x: x[1][1])
    for r in range(3):
        if max(x[1][1] for x in qs[r*4:r*4+4]) >= min(x[1][1] for x in qs[r*4+4:r*4+8]): return None
    rows = [sorted(qs[r*4:r*4+4], key=lambda x: x[1][0]) for r in range(4)]
    return [order_corners(x[0]) for r in rows for x in r]

def order_corners(q):
    """top-left, top-right, bottom-right, bottom-left (image coordinates)."""
    c = q.mean(0); ang = np.arctan2(q[:, 1] - c[1], q[:, 0] - c[0]); q = q[np.argsort(ang)]   # starts at top-left (-135deg)
    return q

def button_crop(img, quad):
    lo = CROP * (1 - 1 / CONTEXT) / 2; hi = CROP - lo
    dst = np.float32([[lo, lo], [hi, lo], [hi, hi], [lo, hi]])
    H = cv2.getPerspectiveTransform(quad.astype(np.float32), dst)
    return cv2.warpPerspective(img, H, (CROP, CROP), flags=cv2.INTER_AREA, borderMode=cv2.BORDER_REPLICATE)
