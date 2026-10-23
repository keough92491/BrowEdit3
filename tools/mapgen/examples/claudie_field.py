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
m.noise(scale=46, amplitude=1.0, octaves=3, persistence=0.45)
z = m.base * 2.3
lvl = np.floor(z)
frac = z - lvl
STEP = 18.0
m.base = STEP * (lvl + _smoothstep((frac - 0.5) / 0.09 + 0.5))
m.noise(scale=7, amplitude=1.2, octaves=2, seed=3)          # a little roughness on the plateaus

# ---- winding path(s), flattened into ramps wherever they cross a terrace
main = [(2, 62)]
for i, x in enumerate(range(14, 100, 14)):
    main.append((x, 50 + 16 * np.sin(i * 1.3 + 0.7) + rng.uniform(-4, 4)))
branch = [main[2], (36, 28), (42, 12), (58, 4)]
for _ in range(3):
    m.path(main, width=3.4, flatten=0.9)
    m.path(branch, width=2.6, flatten=0.9)
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
dirt_mask = (m.masks["path"] > 0.5) | ring
dirt = m.autotile_dirt(dirt_mask, DIRT, EDGE, BLOB)
m.autotile_cliffs(CLIFF, min_slope=0.45, skip_mask=dirt)

# ---- forest clumps and meadows
fn = m._value_noise(13, np.random.default_rng(9))[:W, :H]
m.masks["forest"] = (fn > 0.52).astype(float)
m.masks["meadow"] = 1.0 - m.masks["forest"]
sl = m.slope_map()
cl = (sl > 0.3)
cliffnear = cl.copy()
for _ in range(2):
    cliffnear[1:, :] |= cl[:-1, :]; cliffnear[:-1, :] |= cl[1:, :]; cliffnear[:, 1:] |= cl[:, :-1]; cliffnear[:, :-1] |= cl[:, 1:]
m.masks["cliffnear"] = cliffnear.astype(float)
av = ("water", "path", "dirt")
m.scatter(["roo_payon\\pine3.rsm", "roo_fild\\tree.rsm"], 240, region=(3, 3, 97, 97), min_dist=3.2, footprint=(0.8, 0.8),
          avoid=av, max_slope=0.4, mask="forest", scale_range=(1.0, 1.7), weights=[3, 2])
m.scatter(["roo_fild\\tree.rsm", "roo_payon\\pine3.rsm"], 22, region=(3, 3, 97, 97), min_dist=10, footprint=(0.8, 0.8),
          avoid=av, max_slope=0.4, mask="meadow", scale_range=(1.2, 2.0))
m.scatter(["ROO_fild\\rock4.rsm"], 30, region=(3, 3, 97, 97), min_dist=5, footprint=(1.2, 1.2), avoid=av, max_slope=1.5,
          mask="cliffnear", scale_range=(0.9, 1.8))
m.scatter(["roo_fild\\bush1.rsm"], 45, region=(3, 3, 97, 97), min_dist=4, footprint=(1.0, 1.0), avoid=av, max_slope=0.4,
          scale_range=(2.4, 3.7))
# flower clumps (no GAT)
made = 0
while made < 22:
    u, v = rng.uniform(5, 95), rng.uniform(5, 95)
    x, y = int(u), int(v)
    if m.masks["meadow"][x, y] < 0.5 or any(m.masks[k][x, y] > 0.5 for k in av) or sl[x, y] > 0.3:
        continue
    for _ in range(int(rng.integers(4, 8))):
        du, dv = u + rng.uniform(-2, 2), v + rng.uniform(-2, 2)
        if not (0 < du < W - 1 and 0 < dv < H - 1) or any(m.masks[k][int(du), int(dv)] > 0.5 for k in av):
            continue
        sc = rng.uniform(0.4, 0.6)
        mm = m.place("roo_prtfild\\b1.rsm", du, dv, rot=(0, float(rng.uniform(0, 360)), 0), scale=(sc, sc, sc * 1.15))
        mm.nogat = True
    made += 1
m.set_sun(longitude=40, latitude=50, ambient=(0.7, 0.7, 0.7))

cmds = m.live_commands()
cmds.insert(1, {"cmd": "delete_models", "filter": ""})
out = sys.argv[1] if len(sys.argv) > 1 else "/mnt/user-data/outputs/c005.json"
json.dump({"commands": cmds}, open(out, "w"), separators=(",", ":"))
print(len(m.models), "models,", int(dirt.sum()), "dirt cubes,", len(m.cube_uv), "tile overrides, lake", round(lu), round(lv))
