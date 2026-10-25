#pragma once
// Advanced Texture edit mode: non-destructive overlay painting that is baked into stable 8-bit BMP atlases.
// The GND only ever sees normal texture paths and UVs; the paint layers live in <map>.browtex.json.
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

class Map;
class MapView;
class BrowEdit;

namespace advtex
{
	constexpr int MASK = 32;			// paint mask resolution per ground cell
	constexpr int PAD = 1;				// gutter (pixels) around every baked cell, avoids seams with linear filtering

	struct Img { int w = 0, h = 0; std::vector<uint8_t> px; };	// RGBA

	inline int key(int x, int y) { return (y << 16) | x; }
	inline int keyX(int k) { return k & 0xFFFF; }
	inline int keyY(int k) { return k >> 16; }

	struct PaintLayer { std::string tex; float tiling = 2.0f; };		// tex is relative to data\texture\ (raw cp949), tiling = cells per repeat
	struct Orig { bool valid = false; std::string file; float uv[8] = { 0,0,0,0,0,0,0,0 }; };	// the tile as it was before the first bake
	typedef std::map<int, std::vector<uint8_t>> Masks;					// layer index -> MASK*MASK alpha

	struct Cell
	{
		Masks masks;
		Orig orig;
		int atlas = -1, slot = -1;
		bool hasTile = false;			// the cube owns a private tile that points to the atlas
		bool dirty = true;
	};
	struct Atlas
	{
		int id = 0;
		std::string file;				// as stored in the GND (relative to data\texture\)
		int size = 512, cellPx = 30;
		std::vector<int> slots;			// cell key per slot, -1 = free
		uint64_t hash = 0;				// fingerprint of the last written file
		bool dirty = true;
		bool retired = false;
		std::vector<uint8_t> rgba;		// render cache (not saved)
	};
	struct Project
	{
		std::vector<PaintLayer> layers;
		std::map<int, Cell> cells;
		std::vector<Atlas> atlases;
		int nextAtlasId = 1;
		std::string folder = "advtex";	// below data\texture\, ASCII
		int cellPx = 30;				// 14 / 30 / 62
		int atlasSize = 512;			// 256 / 512 (8 bit BMP, must stay below 1 MB)
		bool autoBake = true;
		bool loaded = false, pending = false;
		std::string path, status;
		int selLayer = 0;
	};

	bool isEmptyCell(const Cell& c);
	std::string mapBase(Map* map);
	std::string atlasFile(Project& P, Map* map, int id);
	// every file operation of a bake goes through here; false = nothing was changed
	bool bake(Map* map, BrowEdit* be, Project& P, bool rebuildAll, std::string& msg);
	void loadProject(Map* map, BrowEdit* be, Project& P);
	void saveProject(Map* map, BrowEdit* be, Project& P);
	void clearImageCache();
	const Img* getImg(const std::string& file);
	std::vector<std::string> unusedAtlases(Map* map, Project& P);
	int deleteUnused(Map* map, BrowEdit* be, Project& P);
	// bake pending changes and write the sidecar (called by the map save)
	void onSave(Map* map, BrowEdit* be);
	void forget(Map* map);
	Project& project(Map* map, BrowEdit* be);

	// UI (AdvTexMode.cpp)
	void showWindow(BrowEdit* be);
	void postRender(MapView* mv, BrowEdit* be);
}
