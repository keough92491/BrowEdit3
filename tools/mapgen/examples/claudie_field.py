"""Terraced field: seamless dirt paths (edge tiles), cliff faces, a lake with a sandy shore, forest clumps.
Textures are the roo_prtf transition set, addressed by index in the open map: #0 grass, #1 cliff face, #2 dirt edge strip,
#3 dirt, #6 dirt blob."""
import sys, os, json
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rogen import MapGen, _smoothstep

W = H = 100
m = MapGen("claudie", W, H, seed=int(os.environ.get("SEED", 5)))
GRASS = m.add_texture("#0"); CLIFF = m.add_texture("#1"); EDGE = m.add_texture("#2")
DIRT = m.add_texture("#3"); BLOB = m.add_texture("#6")
rng = np.random.default_rng(m.seed)

# ---- terrain: a few LARGE level regions (no tiny cliffs), one uniform soft step between them
from scipy import ndimage as ndi
STEP = 30.0
SIG = 1.1                                              # width of a cliff transition
SEEDS = np.random.default_rng(m.seed + 100)
def smooth_noise(scale, rg):
    g = rg.random((int((W + 1) / scale) + 3, int((H + 1) / scale) + 3))
    return ndi.zoom(g, scale, order=3)[:W + 1, :H + 1]
z = smooth_noise(30, SEEDS) + 0.5 * smooth_noise(14, SEEDS)
qs = np.quantile(z, [0.5])
lvl = np.digitize(z, qs)                               # 0,1,2
for _ in range(3):                                     # clean up: majority filter
    cnt = np.stack([ndi.uniform_filter((lvl == k).astype(float), 15) for k in range(2)])
    lvl = cnt.argmax(0)
def drop_small(lvl, min_size=260):
    lvl = lvl.copy()
    for k in range(2):
        lab, n = ndi.label(lvl == k)
        for i in range(1, n + 1):
            reg = lab == i
            if reg.sum() < min_size:
                lvl[reg] = -1
    bad = lvl < 0
    if bad.any():
        idx = ndi.distance_transform_edt(bad, return_distances=False, return_indices=True)
        lvl = lvl[idx[0], idx[1]]
    return lvl
for _ in range(2):
    lvl = drop_small(lvl, 400)
# levels may not jump by 2: a plateau of level 1 between 0 and 2
Hs = ndi.gaussian_filter(lvl * STEP, SIG, mode="nearest")           # soft steps
LV = Hs / STEP
FT = LV - np.floor(LV + 1e-6)                                       # 0..1 inside one step
m.base = Hs.copy()

# ---- paths stay on one level (cliffs are then never cut open) and avoid the cliff foot
def region_dist(mask):
    return ndi.distance_transform_edt(np.pad(mask, 1))[1:-1, 1:-1]       # the map edge counts as a border
def walk(mask, rg, step=13.0, n=14, margin=6.5, start=None, heading=None, avoid=()):
    d = region_dist(mask)
    if start is None:
        cand = np.argwhere(d > max(margin, d.max() * 0.6))
        start = cand[int(rg.integers(len(cand)))]
    pts = [(float(start[0]), float(start[1]))]
    hd = rg.uniform(0, 2 * np.pi) if heading is None else heading
    for _ in range(n):
        offs = sorted(rg.uniform(-1.3, 1.3, 30), key=abs)       # smoothest valid turn first
        for o in offs:
            h2 = hd + o
            nx, ny = pts[-1][0] + step * np.cos(h2), pts[-1][1] + step * np.sin(h2)
            if 2 <= nx <= W - 2 and 2 <= ny <= H - 2 and d[int(nx), int(ny)] > margin and all(
                    (nx - ax) ** 2 + (ny - ay) ** 2 > 64 for ax, ay in list(avoid) + pts[:-2]):
                pts.append((nx, ny)); hd = h2 + rg.uniform(-0.12, 0.12); break
        else:
            break
    return pts
regions = []
for k in range(2):
    lab, n = ndi.label(lvl == k)
    for i in range(1, n + 1):
        regions.append(((lab == i).sum(), k, lab == i))
regions.sort(key=lambda r: -r[0])
m.base += (smooth_noise(9, np.random.default_rng(3)) - 0.5) * 1.2        # a little roughness
# ---- lake in the biggest lowest region
low = [r for r in regions if r[1] == 0] or regions[-1:]
_, lk, lmask = max(low, key=lambda r: r[0])
dl = region_dist(lmask)
lu, lv = np.unravel_index(np.argmax(dl - 0.02 * np.abs(np.arange(W + 1)[:, None] - 50)), dl.shape)
lrad = float(min(12, dl.max() * 0.62))
L0 = float(lk * STEP)
m.lake(float(lu), float(lv), lrad, 10)
m.water(level=L0 - 3.5, amplitude=0.4)
gxx, gyy = np.meshgrid(np.arange(W + 1), np.arange(H + 1), indexing="ij")
lake_zone = np.hypot(gxx - lu, gyy - lv) < lrad + 7

# ---- paths: long winding routes inside a region, away from the lake and from every cliff
import heapq
def route(mask, a, b, cost_extra):
    """cheapest 8-connected way from cell a to cell b through mask"""
    from skimage.graph import route_through_array
    cost = np.where(mask, 1.0 + cost_extra, 1e6)
    path, _ = route_through_array(cost, a, b, fully_connected=True, geometric=True)
    return [tuple(c) for c in path]
def smooth_path(cells, k=4.0, every=6):
    pts = np.array(cells, dtype=float)
    pts[:, 0] = ndi.gaussian_filter1d(pts[:, 0], k, mode="nearest")
    pts[:, 1] = ndi.gaussian_filter1d(pts[:, 1], k, mode="nearest")
    sel = list(range(0, len(pts), every)) + [len(pts) - 1]
    return [(float(pts[i, 0]), float(pts[i, 1])) for i in sel]
paths = []
taken = np.zeros((W + 1, H + 1), dtype=bool)
for (_, k, mask) in regions[:2]:
    mk = mask & ~lake_zone
    d = region_dist(mk)
    ok_m = d > 6.5
    if ok_m.sum() < 200:
        continue
    pts_ok = np.argwhere(ok_m)
    # two far apart cells: pick a random one, then the farthest from it, then the farthest from that
    a0 = pts_ok[int(rng.integers(len(pts_ok)))]
    A = pts_ok[np.argmax(((pts_ok - a0) ** 2).sum(1))]
    B = pts_ok[np.argmax(((pts_ok - A) ** 2).sum(1))]
    wob = ndi.zoom(rng.random((9, 9)), 12, order=3)[:W + 1, :H + 1] * 16.0           # makes the route meander
    cost = wob + np.where(taken, 40.0, 0.0) + np.clip(10.0 - d, 0, 10) * 0.4
    cells = route(ok_m, tuple(A), tuple(B), cost)
    if len(cells) < 20:
        continue
    pl = smooth_path(cells)
    paths.append(pl)
    taken |= ndi.binary_dilation(np.isin(np.arange((W + 1) * (H + 1)).reshape(W + 1, H + 1), []), iterations=0)
    for (cx, cy) in cells:
        taken[max(cx - 6, 0):cx + 7, max(cy - 6, 0):cy + 7] = True
    # a branch from the middle to the farthest ok cell from there
    mid = cells[len(cells) // 2]
    far = pts_ok[np.argmax(((pts_ok - np.array(mid)) ** 2).sum(1) * (~taken[tuple(pts_ok.T)]))]
    cb = route(ok_m, tuple(mid), tuple(far), cost + wob)
    if len(cb) >= 14:
        paths.append(smooth_path(cb))
for pts in paths:
    m.path(pts, width=3.8, flatten=0.4)

# ---- texturing
m.paint_all(GRASS)
wm = m.masks["water"] > 0.5
ring = wm.copy()
ring[1:, :] |= wm[:-1, :]; ring[:-1, :] |= wm[1:, :]; ring[:, 1:] |= wm[:, :-1]; ring[:, :-1] |= wm[:, 1:]
dirt_cubes = (m.masks["path"] > 0.5) | ring
m.masks["dirt"] = dirt_cubes.astype(float)
# material wanted at every cube corner: 0 grass, 1 dirt, 2 rock
mat = np.zeros((W + 1, H + 1), dtype=int)
shore = np.zeros((W + 1, H + 1), dtype=bool)
cw = np.pad(ring, ((1, 1), (1, 1)))          # corner (x,y) touches cubes (x-1..x, y-1..y)
shore = cw[:-1, :-1] | cw[1:, :-1] | cw[:-1, 1:] | cw[1:, 1:]
dirt_c = m.corner_path | shore
mat[dirt_c] = 1
from tilelib import TileLib, render
TEX = {0: "ÃÊ¿ø01", 1: "¾ð´ö01", 2: "ÃÊ¿ø02", 3: "Èë02", 4: "¾ð´ö03", 5: "¾ð´ö02", 6: "ÃÊ¿ø04", 7: "¾ð´ö10", 8: "¾ð´ö11", 9: "ÃÊ¿ø09"}
TEXDIR = os.environ.get("TEXDIR", "/mnt/user-data/uploads/ROO_MAP/data/texture/roo_prtf/")
tex_paths = {i: TEXDIR + "prt_%s.bmp" % n for i, n in TEX.items()}
lib = TileLib(tex_paths, plain={0: 0, 1: 3})
for t in TEX:
    m.add_texture("#%d" % t)
# ---- cliff faces, the prt_fild00 way: the texture runs one quarter per cube along the cliff, v follows the real height
rock = {}
CL = m.add_texture("#1")
nearp = m.corner_path.copy()
for _ in range(2):
    nn = nearp.copy()
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (1, -1), (-1, 1)):
        nn |= np.roll(np.roll(nearp, dx, 0), dy, 1)
    nearp = nn
T = 0.92 - 0.84 * np.clip(FT, 0, 1)                   # 0.08 = top lip (high side) ... 0.92 = bottom lip
hb = Hs
for x in range(W):
    for y in range(H):
        cc = [(x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1)]
        ftv = [FT[a_, b_] for a_, b_ in cc]
        if min(ftv) > 0.96 or max(ftv) < 0.04:
            continue
        if any(nearp[a_, b_] for a_, b_ in cc) or any(shore[a_, b_] for a_, b_ in cc):
            continue
        rock[(x, y)] = cc
def axis_of(x, y):
    c = [hb[x, y], hb[x + 1, y], hb[x, y + 1], hb[x + 1, y + 1]]
    gx = abs((c[1] + c[3]) - (c[0] + c[2]))
    gy = abs((c[2] + c[3]) - (c[0] + c[1]))
    return gx >= gy                                       # True: slope across x, the cliff runs along y
fixed = set(rock)
for (x, y), cc in rock.items():
    along_y = axis_of(x, y)
    uv = []
    for (a_, b_) in cc:
        px, py = a_ - x, b_ - y
        s_ = (((y % 4) + py) if along_y else ((x % 4) + px)) * 0.25
        uv.append((s_, float(T[a_, b_])))
    m.cube_uv[(x, y)] = (CL, uv)
sel = lib.solve(mat, np.random.default_rng(4), fixed=fixed)
for (x, y), (ti, uv) in sel.items():
    m.cube_uv[(x, y)] = (m.add_texture("#%d" % ti), uv)
dirt = dirt_cubes
if os.environ.get("PREVIEW"):
    render(m, tex_paths, 0, 0, W, H, os.environ["PREVIEW"], S=8, seams=True)

# ---- model placement the way prt_fild00 does it: fixed scales, small clusters, a few dense stands
TREE, PINE, BUSH, ROCK = "roo_fild\\tree.rsm", "roo_payon\\pine3.rsm", "roo_fild\\bush1.rsm", "ROO_fild\\rock4.rsm"
rockm = np.zeros((W, H), dtype=bool)
for (x_, y_) in rock:
    rockm[max(x_ - 1, 0):x_ + 2, max(y_ - 1, 0):y_ + 2] = True
sl = m.slope_map()
m._update_water_mask(); m._slope = sl
placed_xy = []

def ok(u, v, slope=0.4, gap=2.2):
    if not (3 < u < W - 3 and 3 < v < H - 3):
        return False
    x, y = int(u), int(v)
    if any(m.masks[k][x, y] > 0.5 for k in ("water", "path", "dirt")) or sl[x, y] > slope or rockm[x, y]:
        return False
    return all((u - pu) ** 2 + (v - pv) ** 2 >= gap * gap for pu, pv in placed_xy)

def put(file, u, v, scale):
    m.place(file, u, v, rot=(0, float(rng.uniform(0, 360)), 0), scale=scale, footprint=(0.8, 0.8))
    placed_xy.append((u, v))

def cluster(files, weights, n, radius, scale_of, cu=None, cv=None, gap=2.2):
    for _ in range(40):
        c_u = cu if cu is not None else rng.uniform(6, W - 6)
        c_v = cv if cv is not None else rng.uniform(6, H - 6)
        if ok(c_u, c_v):
            break
    else:
        return 0
    made = 0
    for _ in range(n * 6):
        if made >= n:
            break
        u, v = rng.normal(c_u, radius), rng.normal(c_v, radius)
        if ok(u, v, gap=gap):
            f = files[int(rng.choice(len(files), p=np.array(weights) / sum(weights)))]
            put(f, u, v, scale_of[f])
            made += 1
    return made

SC = {TREE: (1.95,) * 3, PINE: (1.42,) * 3, BUSH: (3.65,) * 3, ROCK: (1.04,) * 3}
# two dense stands (pines mostly) and many small groups
for _ in range(2):
    cluster([PINE, TREE], [5, 1], 26, 5.5, SC, gap=2.6)
for _ in range(16):
    cluster([TREE, PINE], [3, 2], int(rng.integers(4, 9)), 2.6, SC)
# single trees in the meadows
for _ in range(34):
    cluster([TREE, PINE], [3, 2], 1, 0.1, SC, gap=6)
# bushes in twos and threes, rocks along the cliff rims
for _ in range(10):
    cluster([BUSH], [1], int(rng.integers(1, 4)), 1.6, SC, gap=2.5)
rim = np.argwhere(rockm)
for _ in range(16):
    if len(rim):
        cx, cy = rim[int(rng.integers(len(rim)))]
        for _ in range(8):
            u, v = cx + rng.uniform(-3, 3), cy + rng.uniform(-3, 3)
            if ok(u, v, slope=1.5, gap=3):
                put(ROCK, u, v, SC[ROCK]); break
m.set_sun(longitude=40, latitude=50, ambient=(0.7, 0.7, 0.7))

cmds = m.live_commands()
cmds.insert(1, {"cmd": "delete_models", "filter": ""})
cmds.append({"cmd": "save"})
out = sys.argv[1] if len(sys.argv) > 1 else "/mnt/user-data/outputs/c005.json"
json.dump({"commands": cmds}, open(out, "w"), separators=(",", ":"))
print(len(m.models), "models,", int(dirt.sum()), "dirt cubes,", len(m.cube_uv), "tile overrides, lake", round(lu), round(lv))
