"""novground v2: natural meandering dirt paths (varying width, forks, wear patches) on grass; only tiles are written."""
import sys, os, json
import numpy as np
from scipy import ndimage as ndi
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import tilelib
from tilelib import TileLib
from edges import EdgeField
from PIL import Image

W = H = 200; Wc = Hc = 201
IMG_W, IMG_H = 1250.0, 1160.0
def P(px, py): return (px / IMG_W * W, (1 - py / IMG_H) * H)
SCR = "/tmp/claude-0/-home-claude/bcc60026-74cc-55ec-960a-59e929b4c57f/scratchpad/"
rng = np.random.default_rng(int(os.environ.get("SEED", 21)))

CAMP = P(625, 598)
ROADS = {   # control points (reference pixels); every road starts inside the plaza
 "north": [(625,598),(630,500),(640,420),(622,350),(640,290),(655,230),(640,180),(628,150)],
 "mine":  [(628,330),(600,290),(560,268),(520,250)],
 "west":  [(625,598),(560,590),(490,572),(420,578),(345,590),(292,625),(265,690),(255,750)],
 "east":  [(625,598),(710,595),(780,578),(860,566),(930,556),(960,530),(956,470),(946,430),(945,405)],
 "eastsouth": [(958,545),(962,620),(984,700),(996,770)],
 "south": [(625,598),(622,680),(632,760),(626,830),(596,900),(566,985),(570,1058),(618,1090),(692,1088),(738,1048),(750,985),(736,925),(702,880),(658,850),(630,820)],
}
cx, cy = np.meshgrid(np.arange(Wc), np.arange(Hc), indexing="ij")
def noise1(n, scale, rg):
    a = rg.random(n // scale + 4)
    return ndi.zoom(a, scale, order=3)[:n] - 0.5
def resample(pts, step=0.7):
    p = np.array(pts, float); out = [p[0]]
    for a, b in zip(p[:-1], p[1:]):
        L = np.hypot(*(b - a)); n = max(int(L / step), 1)
        for t in np.arange(1, n + 1) / n: out.append(a * (1 - t) + b * t)
    return np.array(out)
def smooth(p, passes=10):
    p = p.copy()
    for _ in range(passes): p[1:-1] = (p[:-2] + 2 * p[1:-1] + p[2:]) / 4
    return p
field = np.full((Wc, Hc), -9.0)
centre_lines = []
for name, pts in ROADS.items():
    q = smooth(resample([P(*a) for a in pts], 1.0), 6)
    q = resample(q, 0.6)
    n = len(q)
    t = np.gradient(q, axis=0); t /= np.linalg.norm(t, axis=1, keepdims=True) + 1e-9
    nrm = np.stack([-t[:, 1], t[:, 0]], 1)
    # meander: slow wander plus a finer wobble, tapered to 0 where the road joins the plaza
    off = (noise1(n, 55, rng) * 9.0 + noise1(n, 18, rng) * 3.2)
    ramp = np.clip(np.hypot(q[:, 0] - CAMP[0], q[:, 1] - CAMP[1]) / 14.0 - 0.7, 0, 1)
    q = q + nrm * (off * ramp)[:, None]
    q = smooth(q, 6)
    w0 = 5.2 if name in ("north", "west", "east", "south") else 4.0
    wid = w0 * (1.0 + noise1(n, 40, rng) * 0.8 + noise1(n, 12, rng) * 0.35)
    wid = np.clip(wid, w0 * 0.65, w0 * 1.5)
    centre_lines.append((q, wid))
    for (x, y), w in zip(q, wid):                         # distance field = union of discs along the centreline
        x0, x1 = int(max(x - w - 2, 0)), int(min(x + w + 3, Wc)); y0, y1 = int(max(y - w - 2, 0)), int(min(y + w + 3, Hc))
        d = np.hypot(cx[x0:x1, y0:y1] - x, cy[x0:x1, y0:y1] - y)
        field[x0:x1, y0:y1] = np.maximum(field[x0:x1, y0:y1], w / 2 - d)
# worn patches / small widenings next to the roads (natural trampled spots)
for q, wid in centre_lines:
    for _ in range(max(2, len(q) // 40)):
        i = int(rng.integers(len(q))); a = rng.random() * 6.28
        px_, py_ = q[i] + 2.4 * np.array([np.cos(a), np.sin(a)]); r = 1.1 + rng.random() * 1.2
        d = np.hypot(cx - px_, cy - py_); field = np.maximum(field, r - d + 0.3)
# irregular plaza
ang = np.arctan2(cy - CAMP[1], cx - CAMP[0])
rad = 14.5 + 1.8 * np.sin(3 * ang + 0.7) + 1.1 * np.sin(5 * ang + 2.0) + 0.7 * np.sin(8 * ang)
field = np.maximum(field, rad - np.hypot(cx - CAMP[0], cy - CAMP[1]))
field = ndi.gaussian_filter(field, 0.7)
EF = EdgeField(field, Wc, Hc, np.random.default_rng(11))
mat = np.zeros((Wc, Hc), int); mat[EF.dd > 0] = 1

def classify_hd(img):        # the HD client textures: olive grass (r<g) vs golden dirt (r>>g)
    a = np.asarray(img.convert("RGB"), dtype=int)
    d = ndi.gaussian_filter((a[..., 0] - a[..., 1]).astype(float), 2.5, mode="wrap")
    return (d > 13).astype(np.int8)
tilelib.classify = classify_hd
td = "/mnt/user-data/uploads/texture/ÇÊµå¹Ù´Ú/"
PLAIN_DIRT = "/mnt/user-data/uploads/ROO_MAP/data/texture/roo_prtf/prt_Èë03.bmp"
names = {0: "prt_ÃÊ¿ø01.bmp", 1: "prt_ÃÊ¿ø02.bmp", 2: "prt_ÃÊ¿ø03.bmp", 3: "prt_ÃÊ¿ø04.bmp", 4: "prt_ÃÊ¿ø05.bmp"}
lib = TileLib({**{i: td + n for i, n in names.items()}, 5: PLAIN_DIRT}, plain={0: 0, 1: 5})
sel = lib.solve(mat, np.random.default_rng(4), fixed=set(), noise=float(os.environ.get("NOISE", 0.2)))
edgeuv = EF.edge_cubes((0, W), (0, H), skip=set())
tiles, used = [], {}
cube_uv = {}
for x in range(W):
    for y in range(H):
        if (x, y) in edgeuv: uv, ti = edgeuv[(x, y)], 1
        else: ti, uv = sel[(x, y)]
        used[ti] = used.get(ti, 0) + 1; cube_uv[(x, y)] = (ti, uv)
        tiles.append([x, y, ti] + [round(c, 4) for (a, b) in uv for c in (a, b)])
print("tiles", len(tiles), used, "dirt cubes", int((mat[:W, :H] == 1).sum()))
json.dump({"commands": [{"cmd": "tiles", "list": tiles}]}, open("/mnt/user-data/outputs/ng3.json", "w"), separators=(",", ":"))
# previews: schematic and textured render
pv = np.zeros((W, H, 3), np.uint8); pv[:] = (110, 170, 50); pv[mat[:W, :H] == 1] = (225, 205, 150)
Image.fromarray(pv.transpose(1, 0, 2)[::-1]).resize((600, 600), Image.NEAREST).save(SCR + "ng_pv.png")
S = 10
tex = {i: np.asarray(Image.open(td + n).convert("RGB").resize((128, 128))) for i, n in names.items()}
tex[5] = np.asarray(Image.open(PLAIN_DIRT).convert("RGB").resize((128, 128)))
def render(x0, y0, w, h, out):
    img = np.zeros((h * S, w * S, 3), np.uint8)
    a = (np.arange(S) + 0.5) / S; fa, fb = np.meshgrid(a, a)
    for x in range(x0, x0 + w):
        for y in range(y0, y0 + h):
            ti, uv = cube_uv[(x, y)]
            u = [c[0] for c in uv]; v = [c[1] for c in uv]
            uu = (u[0] * (1 - fa) + u[1] * fa) * (1 - fb) + (u[2] * (1 - fa) + u[3] * fa) * fb
            vv = (v[0] * (1 - fa) + v[1] * fa) * (1 - fb) + (v[2] * (1 - fa) + v[3] * fa) * fb
            px = np.clip(((uu % 1.0) * 128).astype(int), 0, 127); py = np.clip(((vv % 1.0) * 128).astype(int), 0, 127)
            r = (h - 1 - (y - y0)) * S
            img[r:r + S, (x - x0) * S:(x - x0 + 1) * S] = tex[ti][py[::-1], px]
    Image.fromarray(img).save(out)
cxi, cyi = int(CAMP[0]), int(CAMP[1])
render(cxi - 40, cyi - 25, 80, 50, SCR + "ng_r1.png")
S = 24
render(int(CAMP[0]) - 36, int(CAMP[1]) - 4, 26, 10, SCR + "ng_r2.png")
print("edge cubes", len(edgeuv))
