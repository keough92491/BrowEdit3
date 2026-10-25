#include <glad/gl.h>
#include "CliffTool.h"
#include <browedit/BrowEdit.h>
#include <browedit/Map.h>
#include <browedit/MapView.h>
#include <browedit/Node.h>
#include <browedit/components/Gnd.h>
#include <browedit/components/GndRenderer.h>
#include <browedit/actions/GroupAction.h>
#include <browedit/actions/CubeHeightChangeAction.h>
#include <browedit/actions/CubeTileChangeAction.h>
#include <browedit/actions/TileNewAction.h>
#include <browedit/gl/Texture.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

namespace cliff
{
	Settings& settings() { static Settings s; return s; }

	namespace
	{
		float hash2(int x, int y, int seed)
		{
			unsigned int h = (unsigned int)(x * 374761393 + y * 668265263 + seed * 2147483647u);
			h = (h ^ (h >> 13)) * 1274126177u;
			h ^= h >> 16;
			return (h & 0xFFFFFF) / (float)0xFFFFFF;
		}
		float vnoise(float x, float y, int seed)
		{
			int xi = (int)std::floor(x), yi = (int)std::floor(y);
			float fx = x - xi, fy = y - yi;
			fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
			float a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed), c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
			return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
		}
		float fbm(float x, float y, int seed)
		{
			return 0.65f * vnoise(x, y, seed) + 0.35f * vnoise(x * 2.3f + 17.0f, y * 2.3f + 5.0f, seed + 7);
		}

		// stroke state
		bool stroking = false;
		std::map<Gnd::Cube*, float[4]> origHeights;
		std::map<Gnd::Cube*, float[4]> weights;
		std::vector<glm::ivec2> touched;
		glm::vec2 lastCentre(-1000.0f);
		int strokeSeed = 1;

		// corner i of cube (x,y) in tile units (x to the right, z the same axis as the world z / 10)
		glm::vec2 cornerPos(Gnd* gnd, int x, int y, int i)
		{
			return glm::vec2((float)x + (i % 2), (float)(gnd->height - y) + (i / 2 == 0 ? 1.0f : 0.0f));
		}

		float terrain(Gnd* gnd, float tx, float tz)		// world y at tile coordinates
		{
			int x = std::clamp((int)std::floor(tx), 0, gnd->width - 1);
			int y = std::clamp(gnd->height - 1 - (int)std::floor(tz), 0, gnd->height - 1);
			float fx = std::clamp(tx - x, 0.0f, 1.0f), fz = std::clamp(tz - (gnd->height - 1 - y), 0.0f, 1.0f);
			auto c = gnd->cubes[x][y];
			float top = c->h3 + (c->h4 - c->h3) * fx, bottom = c->h1 + (c->h2 - c->h1) * fx;
			return -(top + (bottom - top) * fz);
		}

		float levelWeight(const Settings& s, float d)		// d = noisy distance, returns 0..1
		{
			float w;
			if (s.profile == 1)
			{
				if (d >= s.radius) return 0.0f;
				int n = std::max(1, s.terraces);
				int level = std::min(n, 1 + (int)std::floor((1.0f - d / s.radius) * n * 0.999f));
				return level / (float)n;
			}
			w = std::clamp((s.radius - d) / std::max(0.01f, s.edgeWidth), 0.0f, 1.0f);
			w = w * w * (3 - 2 * w);
			if (s.terraces > 1 && w > 0.001f)
				w = std::ceil(w * s.terraces - 0.0001f) / s.terraces;
			return w;
		}

		float noisyDistance(const Settings& s, const glm::vec2& p, const glm::vec2& c)
		{
			float d = glm::length(p - c);
			float n = fbm(p.x * s.noiseScale, p.y * s.noiseScale, strokeSeed) - 0.5f;
			return d + n * 2.0f * s.roughness * s.radius * 0.5f;
		}

		void stamp(Gnd* gnd, GndRenderer* gr, const glm::vec2& c)
		{
			auto& s = settings();
			float reach = s.radius * (1.0f + s.roughness * 0.5f) + 2.0f;
			int x0 = std::max(0, (int)std::floor(c.x - reach)), x1 = std::min(gnd->width - 1, (int)std::ceil(c.x + reach));
			int y0 = std::max(0, (int)std::floor(gnd->height - c.y - reach)), y1 = std::min(gnd->height - 1, (int)std::ceil(gnd->height - c.y + reach));
			for (int x = x0; x <= x1; x++)
				for (int y = y0; y <= y1; y++)
				{
					auto cube = gnd->cubes[x][y];
					float w[4];
					bool any = false;
					if (s.profile == 1)
					{
						glm::vec2 centre((float)x + 0.5f, (float)(gnd->height - y) + 0.5f);
						float ww = levelWeight(s, noisyDistance(s, centre, c));
						for (int i = 0; i < 4; i++) w[i] = ww;
						any = ww > 0;
					}
					else
						for (int i = 0; i < 4; i++)
						{
							w[i] = levelWeight(s, noisyDistance(s, cornerPos(gnd, x, y, i), c));
							any |= w[i] > 0;
						}
					if (!any) continue;
					if (origHeights.find(cube) == origHeights.end())
					{
						for (int i = 0; i < 4; i++) { origHeights[cube][i] = cube->heights[i]; weights[cube][i] = 0.0f; }
						touched.push_back(glm::ivec2(x, y));
					}
					for (int i = 0; i < 4; i++)
					{
						weights[cube][i] = std::max(weights[cube][i], w[i]);
						float o = origHeights[cube][i], ww = weights[cube][i];
						if (s.mode == 0) cube->heights[i] = o - s.height * ww;
						else if (s.mode == 1) cube->heights[i] = o + s.height * ww;
						else cube->heights[i] = o + (s.height - o) * ww;
					}
					cube->calcNormal();
					gr->setChunkDirty(x, y);
				}
		}

		int textureIndexFor(Gnd* gnd, const std::string& file)
		{
			for (int i = 0; i < (int)gnd->textures.size(); i++)
				if (gnd->textures[i]->file == file) return i;
			return -1;
		}

		void finish(MapView* mv, BrowEdit* browEdit, Map* map, Gnd* gnd, GndRenderer* gr)
		{
			auto& s = settings();
			if (origHeights.empty()) { stroking = false; return; }
			auto ga = new GroupAction("Cliff brush");
			std::map<Gnd::Cube*, float[4]> newHeights;
			for (auto& kv : origHeights)
				for (int i = 0; i < 4; i++)
					newHeights[kv.first][i] = kv.first->heights[i];
			ga->addAction(new CubeHeightChangeAction<Gnd, Gnd::Cube>(origHeights, newHeights, touched));

			std::mt19937 rng((unsigned)strokeSeed * 7919u + (unsigned)touched.size());
			auto pick = [&](const std::vector<std::string>& set) -> int {
				std::vector<int> idx;
				for (auto& f : set) { int t = textureIndexFor(gnd, f); if (t >= 0) idx.push_back(t); }
				if (idx.empty()) return -1;
				return idx[rng() % idx.size()];
			};
			int nextId = (int)gnd->tiles.size();

			std::set<std::pair<int, int>> region;
			for (auto& t : touched)
				for (int dx = -1; dx <= 1; dx++)
					for (int dy = -1; dy <= 1; dy++)
					{
						int x = t.x + dx, y = t.y + dy;
						if (x >= 0 && y >= 0 && x < gnd->width && y < gnd->height) region.insert({ x, y });
					}

			// cliff faces
			if (s.profile == 1 && s.buildWalls && !s.wallTextures.empty() && !gnd->lightmaps.empty())
			{
				auto makeWall = [&](int x, int y, int slot)
				{
					auto cube = gnd->cubes[x][y];
					int tex = pick(s.wallTextures);
					if (tex < 0) return;
					auto t = new Gnd::Tile();
					t->color = glm::ivec4(255, 255, 255, 255);
					t->textureIndex = (short)tex;
					t->lightmapIndex = 0;
					float u0 = 0.0f, u1 = 1.0f;
					if (s.randomSlice) { u0 = (rng() % 4) * 0.25f; u1 = u0 + 0.25f; }
					if (rng() % 2) std::swap(u0, u1);
					t->v1 = glm::vec2(u0, 1); t->v2 = glm::vec2(u1, 1); t->v3 = glm::vec2(u0, 0); t->v4 = glm::vec2(u1, 0);
					if (slot == 1) ga->addAction(new CubeTileChangeAction(glm::ivec2(x, y), cube, cube->tileUp, nextId, cube->tileSide));
					else ga->addAction(new CubeTileChangeAction(glm::ivec2(x, y), cube, cube->tileUp, cube->tileFront, nextId));
					cube->tileIds[slot] = nextId++;
					ga->addAction(new TileNewAction(t));
				};
				const float eps = 0.01f;
				for (auto& r : region)
				{
					int x = r.first, y = r.second;
					auto c = gnd->cubes[x][y];
					if (c->tileUp < 0) continue;
					if (x < gnd->width - 1)
					{
						auto n = gnd->cubes[x + 1][y];
						bool diff = std::fabs(c->h2 - n->h1) > eps || std::fabs(c->h4 - n->h3) > eps;
						if (diff && c->tileSide == -1 && n->tileUp >= 0) makeWall(x, y, 2);
					}
					if (y < gnd->height - 1)
					{
						auto n = gnd->cubes[x][y + 1];
						bool diff = std::fabs(c->h3 - n->h1) > eps || std::fabs(c->h4 - n->h2) > eps;
						if (diff && c->tileFront == -1 && n->tileUp >= 0) makeWall(x, y, 1);
					}
				}
			}
			// walls that are flat now
			for (auto& r : region)
			{
				int x = r.first, y = r.second;
				auto c = gnd->cubes[x][y];
				const float eps = 0.01f;
				if (x < gnd->width - 1 && c->tileSide != -1)
				{
					auto n = gnd->cubes[x + 1][y];
					if (std::fabs(c->h2 - n->h1) <= eps && std::fabs(c->h4 - n->h3) <= eps && origHeights.count(c) + origHeights.count(n))
					{
						ga->addAction(new CubeTileChangeAction(glm::ivec2(x, y), c, c->tileUp, c->tileFront, -1));
						c->tileSide = -1;
					}
				}
				if (y < gnd->height - 1 && c->tileFront != -1)
				{
					auto n = gnd->cubes[x][y + 1];
					if (std::fabs(c->h3 - n->h1) <= eps && std::fabs(c->h4 - n->h2) <= eps && origHeights.count(c) + origHeights.count(n))
					{
						ga->addAction(new CubeTileChangeAction(glm::ivec2(x, y), c, c->tileUp, -1, c->tileSide));
						c->tileFront = -1;
					}
				}
			}
			// slope texture: steep ground gets rock
			if (s.profile == 0 && s.texSlope && !s.slopeTextures.empty())
			{
				for (auto& t : touched)
				{
					auto c = gnd->cubes[t.x][t.y];
					if (c->tileUp < 0) continue;
					float mn = std::min(std::min(c->h1, c->h2), std::min(c->h3, c->h4)), mx = std::max(std::max(c->h1, c->h2), std::max(c->h3, c->h4));
					if (mx - mn < 2.0f) continue;
					int tex = pick(s.slopeTextures);
					if (tex < 0) continue;
					auto nt = new Gnd::Tile(*gnd->tiles[c->tileUp]);
					nt->textureIndex = (short)tex;
					ga->addAction(new CubeTileChangeAction(t, c, nextId, c->tileFront, c->tileSide));
					c->tileUp = nextId++;
					ga->addAction(new TileNewAction(nt));
				}
			}

			for (auto& r : region)
			{
				gnd->cubes[r.first][r.second]->calcNormals(gnd, r.first, r.second);
				gr->setChunkDirty(r.first, r.second);
			}
			map->doAction(ga, browEdit);
			origHeights.clear(); weights.clear(); touched.clear();
			stroking = false;
		}
	}

	void update(MapView* mv, BrowEdit* browEdit, Map* map, Gnd* gnd, GndRenderer* gr, const glm::vec3& mouse3D)
	{
		auto& s = settings();
		bool valid = mouse3D.x >= 0 && mouse3D.z >= 0 && mouse3D.x <= gnd->width * 10.0f && mouse3D.z <= gnd->height * 10.0f;
		glm::vec2 c(mouse3D.x / 10.0f, mouse3D.z / 10.0f);
		auto& io = ImGui::GetIO();

		if (valid)
		{
			ImGui::Begin("Statusbar");
			ImGui::Text("Cliff brush: click or drag. Mouse wheel + Alt = radius, Shift flips raise/lower");
			ImGui::End();
			if (io.KeyAlt && io.MouseWheel != 0)
				s.radius = std::clamp(s.radius + io.MouseWheel * 0.5f, 0.5f, 40.0f);
		}
		if (valid && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyAlt)
		{
			if (!stroking)
			{
				stroking = true;
				strokeSeed = s.seed;
				origHeights.clear(); weights.clear(); touched.clear();
				lastCentre = glm::vec2(-1000.0f);
			}
			if (glm::length(c - lastCentre) >= 0.5f)
			{
				int savedMode = s.mode;
				if (io.KeyShift && s.mode < 2) s.mode = 1 - s.mode;
				stamp(gnd, gr, c);
				s.mode = savedMode;
				lastCentre = c;
			}
		}
		if (stroking && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			// the shift flip was applied while stamping; the stored heights are what counts
			finish(mv, browEdit, map, gnd, gr);
		}

		if (valid)		// brush outline (noisy edge as it will be applied)
		{
			glUseProgram(0);
			glLineWidth(2.0f);
			for (int pass = 0; pass < 2; pass++)
			{
				if (pass == 0) glColor4f(1.0f, 0.55f, 0.1f, 1.0f); else glColor4f(1.0f, 1.0f, 1.0f, 0.6f);
				glBegin(GL_LINE_LOOP);
				const int N = 96;
				for (int i = 0; i < N; i++)
				{
					float a = i * 6.2831853f / N;
					glm::vec2 dir(std::cos(a), std::sin(a));
					// find the radius where the noisy distance reaches the brush radius (edge of the brush), inner = full strength
					float target = pass == 0 ? s.radius : std::max(0.0f, s.radius - (s.profile == 0 ? s.edgeWidth : 0.0f));
					float r = target;
					for (int it = 0; it < 4; it++)
					{
						glm::vec2 p = c + dir * r;
						float n = fbm(p.x * s.noiseScale, p.y * s.noiseScale, s.seed) - 0.5f;
						r = std::max(0.0f, target - n * 2.0f * s.roughness * s.radius * 0.5f);
					}
					glm::vec2 p = c + dir * r;
					glVertex3f(p.x * 10.0f, terrain(gnd, p.x, p.y) + 1.0f, p.y * 10.0f);
				}
				glEnd();
			}
			glLineWidth(1.0f);
			glColor4f(1, 1, 1, 1);
		}
	}

	void showOptions(BrowEdit* browEdit, MapView* mv)
	{
		auto& s = settings();
		auto gnd = mv->map->rootNode->getComponent<Gnd>();
		ImGui::Text("Brush: raise/lower/set terrain in a circle with a natural edge.");
		ImGui::RadioButton("Raise", &s.mode, 0); ImGui::SameLine();
		ImGui::RadioButton("Lower", &s.mode, 1); ImGui::SameLine();
		ImGui::RadioButton("Set height", &s.mode, 2);
		ImGui::SliderFloat("Radius (tiles)", &s.radius, 0.5f, 40.0f);
		ImGui::InputFloat(s.mode == 2 ? "Target height" : "Cliff height", &s.height, 1.0f, 5.0f, "%.2f");
		ImGui::Separator();
		ImGui::RadioButton("Sloped cliff", &s.profile, 0); ImGui::SameLine();
		ImGui::RadioButton("Sheer cliff (walls)", &s.profile, 1);
		if (s.profile == 0)
			ImGui::SliderFloat("Slope width (tiles)", &s.edgeWidth, 0.1f, 15.0f);
		ImGui::SliderInt("Terraces", &s.terraces, 1, 8);
		ImGui::SliderFloat("Edge roughness", &s.roughness, 0.0f, 1.5f);
		ImGui::SliderFloat("Curve size", &s.noiseScale, 0.05f, 1.5f);
		ImGui::InputInt("Shape seed", &s.seed);
		ImGui::SameLine();
		if (ImGui::Button("Random")) s.seed = (int)(ImGui::GetTime() * 1000.0) % 100000;
		ImGui::Separator();
		auto setEditor = [&](const char* title, std::vector<std::string>& set)
		{
			ImGui::PushID(title);
			ImGui::Text("%s", title);
			for (int i = 0; i < (int)set.size(); i++)
			{
				ImGui::PushID(i);
				if (ImGui::SmallButton("x")) { set.erase(set.begin() + i); ImGui::PopID(); break; }
				ImGui::SameLine();
				ImGui::TextUnformatted(set[i].c_str());
				ImGui::PopID();
			}
			if (ImGui::Button("Add texture selected in the texture picker") && gnd && mv->textureSelected >= 0 && mv->textureSelected < (int)gnd->textures.size())
			{
				auto& f = gnd->textures[mv->textureSelected]->file;
				if (std::find(set.begin(), set.end(), f) == set.end()) set.push_back(f);
			}
			ImGui::PopID();
		};
		if (s.profile == 1)
		{
			ImGui::Checkbox("Build cliff walls", &s.buildWalls);
			ImGui::Checkbox("Random width slice per wall", &s.randomSlice);
			setEditor("Cliff face textures (random pick)", s.wallTextures);
		}
		else
		{
			ImGui::Checkbox("Texture steep slopes", &s.texSlope);
			setEditor("Slope textures (random pick)", s.slopeTextures);
		}
		ImGui::TextWrapped("Pick a texture in the texture picker window, then press Add. Shift while painting flips raise/lower. Alt + wheel changes the radius. The whole stroke is one undo step; cliff faces appear when you release the mouse.");
	}
}
