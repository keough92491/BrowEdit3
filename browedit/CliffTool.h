#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

class BrowEdit;
class MapView;
class Map;
class Gnd;
class GndRenderer;

// Cliff brush for the Height edit mode (doodle): circular brush that raises / lowers / sets terrain with a natural, noisy edge,
// optionally as a sheer cliff with generated walls and a texture set for the cliff face and the slope.
namespace cliff
{
	struct Settings
	{
		bool active = false;		// cliff brush instead of the height doodle brush
		int mode = 0;			// 0 raise, 1 lower, 2 set to height
		float radius = 5.0f;		// tiles
		float height = 20.0f;
		int profile = 0;		// 0 sloped, 1 sheer (walls)
		float edgeWidth = 2.0f;	// tiles, sloped profile
		float roughness = 0.35f;	// how much noise bends the edge
		float noiseScale = 0.35f;	// noise frequency (per tile)
		int terraces = 1;
		int seed = 1;
		bool buildWalls = true;
		bool randomSlice = true;
		bool texSlope = true;
		std::vector<std::string> wallTextures;
		std::vector<std::string> slopeTextures;
		// ribbon texturing (sloped cliffs): one rock-band texture with continuous UVs along the cliff curve (u along, v across), no seams between tiles
		bool ribbon = true;
		std::string ribbonTex;
		float ribbonPeriod = 5.0f;		// cubes per texture repeat along the cliff
		float ribbonTop = 0.08f, ribbonBottom = 0.92f;		// where the rock band sits in the texture (v)
		bool ribbonFlip = false;
		// smart edge texturing (sheer cliffs): ground pieces that blend rock into grass along the cliff
		bool smartEdges = true;
		std::string pieceTex[3];		// 0 straight edge, 1 rounded corner, 2 diagonal
		int pieceRot[3] = { 0, 0, 0 };	// extra quarter turns
		bool pieceFlip[3] = { false, false, false };
	};
	Settings& settings();
	void showOptions(BrowEdit* browEdit, MapView* mapView);
	void update(MapView* mv, BrowEdit* browEdit, Map* map, Gnd* gnd, GndRenderer* gr, const glm::vec3& mouse3D);
}
