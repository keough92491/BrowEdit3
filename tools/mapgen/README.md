# rogen - Ragnarok map generator for BrowEdit3

Python (numpy only) library that writes `.rsw`, `.gnd`, `.gat` and `.rsw.extra.json` files which open in BrowEdit3.
Claude uses it to build maps from a description: terrain (noise, hills, rivers, lakes, plateaus, cliffs), textures,
water, paths, scattered models (trees, rocks), buildings, lights, sounds, effects and a GAT that matches.

## Workflow
1. In BrowEdit3: **File -> Export asset catalog (for Claude)**. This writes `catalog.json` next to BrowEdit3.exe,
   a list of every model and texture your client has. Claude searches it (`python catalog.py catalog.json tree`) and
   only uses names that exist.
2. Ask Claude for a map. It writes a short Python script (see `examples/village.py`), runs it and hands you the files.
3. Open the `.rsw` in BrowEdit3, then:
   * **Object Edit -> Smart GAT: recalculate all models** - exact GAT for every model (the generator only knows the
     rough footprint Claude gave it). After this, models keep following their GAT when you move or delete them.
   * Run the lightmapper (shadows), then save. Saving recalculates the quadtree.

## Coordinates
1 cube = 1 terrain square = 10 world units = 2x2 GAT cells. Positions are `(u, v)`, u west->east, v north->south,
`0..width` / `0..height`. Heights are world units, positive = up.

## Things to know
* GAT types used: 0 walkable, 1 blocked (models, deep water), 3 shallow water, 5 cliff.
* Cliffs: `terrace_rect()` raises/lowers whole cubes, the height step becomes a wall (`set_wall_texture`).
* Footprints (`footprint=(w, d)` in cubes) only exist to get a rough GAT; the recalculate command in BrowEdit makes it exact.
* Lightmaps start empty (no shadows) until BrowEdit3's lightmapper runs.
