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

# ---- terraces: smooth noise, quantised in soft steps so the contour lines become cliffs
m.noise(scale=float(os.environ.get('TSCALE', 34)), amplitude=1.0, octaves=3, persistence=0.45)
z = np.maximum(m.base * 2.3, -0.95)        # the lowest terrace is one broad flat floor (room for a lake)
lvl = np.floor(z)
frac = z - lvl
FT = np.clip((frac - 0.5) / 0.2 + 0.5, 0, 1)      # 0..1 position inside one terrace step
FT0 = FT.copy()
FTU = (frac - 0.5) / 0.2 + 0.5                       # not clipped: -0.5..1.5 is the cliff face including its grass lips
STEP = 30.0
m.base = STEP * (lvl + _smoothstep((frac - 0.5) / 0.2 + 0.5))
m.noise(scale=7, amplitude=1.2, octaves=2, seed=3)          # a little roughness on the plateaus

# ---- winding path(s), flattened into ramps wherever they cross a terrace
y = float(rng.uniform(30, 70))
main = [(2, y)]
for x in range(16, 100, 14):
    y = float(np.clip(y + rng.uniform(-17, 17), 14, 86))
    main.append((x, y))
branches = []
for k in rng.choice(np.arange(1, len(main) - 1), size=2, replace=False):
    bx, by = main[int(k)]
    up = by > 50 if rng.random() < 0.3 else by < 50          # go to the roomier side
    ty = 3 if up else 97
    branches.append([(bx, by), (bx + rng.uniform(-14, 14), (by + ty) / 2 + rng.uniform(-6, 6)), (bx + rng.uniform(-16, 16), ty)])
for _ in range(3):
    m.path(main, width=3.8, flatten=0.9)
    for br in branches:
        m.path(br, width=3.0, flatten=0.9)
m.smooth(1)

# ---- lake on the lowest terrace, so one water level does not flood the rest
L0 = float(m.base.min())
slope = m.slope_map()
best = None
for _ in range(600):
    u, v = rng.uniform(15, 85), rng.uniform(15, 85)
    x, y = int(u), int(v)
    if m.base[x, y] > L0 + 4:
        continue
    if m.masks["path"][max(x - 9, 0):x + 10, max(y - 9, 0):y + 10].any():
        continue
    sc = slope[max(x - 9, 0):x + 10, max(y - 9, 0):y + 10].mean() + 0.02 * m.base[max(x - 9, 0):x + 10, max(y - 9, 0):y + 10].mean()
    if best is None or sc < best[0]:
        best = (sc, u, v)
if best is None:       # no low flat spot: fall back to a pond anywhere, water only level with its rim
    best = (0, 50.0, 50.0); L0 = m.height_at(50, 50)
_, lu, lv = best
m.lake(lu, lv, 12, 10)
m.water(level=L0 - 3.5, amplitude=0.4)

# ---- texturing
m.paint_all(GRASS)
wm = m.masks["water"] > 0.5
ring = wm.copy()
ring[1:, :] |= wm[:-1, :]; ring[:-1, :] |= wm[1:, :]; ring[:, 1:] |= wm[:, :-1]; ring[:, :-1] |= wm[:, 1:]
dirt_cubes = (m.masks["path"] > 0.5) | ring
m.masks["dirt"] = dirt_cubes.astype(float)
# material wanted at every cube corner: 0 grass, 1 dirt, 2 rock
mat = np.zeros((W + 1, H + 1), dtype=int)
b = m.base
steep = np.zeros_like(b, dtype=bool)
for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
    sh = np.roll(np.roll(b, dx, 0), dy, 1)
    steep |= np.abs(b - sh) > 5.0
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
# ---- cliff faces: ONE continuous uv field (shared corners get the same uv), t across the step, s along the contour
slope_c = m.slope_map()
rock = {}
CL = m.add_texture("#1")
nearp = m.corner_path.copy()
for _ in range(2):
    nn = nearp.copy()
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (1, -1), (-1, 1)):
        nn |= np.roll(np.roll(nearp, dx, 0), dy, 1)
    nearp = nn
T = np.clip(1.0 - (FTU + 0.5) / 2.0, 0.0, 1.0)          # v across the face, 0 = top lip (high side), 1 = bottom lip
hb = m.base
for x in range(W):
    for y in range(H):
        cc = [(x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1)]
        tv = [T[a_, b_] for a_, b_ in cc]
        if min(tv) >= 0.93 or max(tv) <= 0.07 or max(tv) - min(tv) < 0.1:
            continue
        if any(nearp[a_, b_] for a_, b_ in cc) or any(shore[a_, b_] for a_, b_ in cc):
            continue
        rock[(x, y)] = cc
# which way does each cliff run? (prt_fild00 maps one quarter of the texture per cube along the cliff)
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
sl = m.slope_map()
m._update_water_mask(); m._slope = sl
placed_xy = []

def ok(u, v, slope=0.4, gap=2.2):
    if not (3 < u < W - 3 and 3 < v < H - 3):
        return False
    x, y = int(u), int(v)
    if any(m.masks[k][x, y] > 0.5 for k in ("water", "path", "dirt")) or sl[x, y] > slope or mat[x, y] == 2 or mat[x + 1, y + 1] == 2:
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
rim = np.argwhere(mat == 2)
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
