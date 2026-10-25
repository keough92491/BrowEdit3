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
		std::vector<glm::vec2> centres;

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


		struct Poly { std::vector<glm::vec2> p; std::vector<float> s; bool closed = false; float L = 0, k = 0, period = 0; };

		float fieldAt(const Settings& st, const glm::vec2& p)
		{
			float m = -1e9f;
			for (auto& c : centres) m = std::max(m, st.radius - noisyDistance(st, p, c));
			return m;
		}

		// contours of the brush field (marching squares), smoothed and resampled
		std::vector<Poly> contours(const Settings& st, float tx0, float tz0, float tx1, float tz1)
		{
			const int R = 3;
			int nx = (int)std::ceil((tx1 - tx0) * R) + 1, nz = (int)std::ceil((tz1 - tz0) * R) + 1;
			std::vector<float> F((size_t)nx * nz);
			auto at = [&](int i, int j) -> float& { return F[(size_t)i * nz + j]; };
			for (int i = 0; i < nx; i++) for (int j = 0; j < nz; j++) at(i, j) = fieldAt(st, glm::vec2(tx0 + (float)i / R, tz0 + (float)j / R));
			auto key = [&](int i, int j, int dir) -> long long { return ((long long)i * nz + j) * 2 + dir; };
			std::map<long long, glm::vec2> pos;
			std::map<long long, std::vector<long long>> adj;
			auto crossing = [&](int i, int j, int dir) -> long long
			{
				long long k = key(i, j, dir);
				if (!pos.count(k))
				{
					int i2 = dir == 0 ? i + 1 : i, j2 = dir == 0 ? j : j + 1;
					float a = at(i, j), b = at(i2, j2), t = (a == b) ? 0.5f : a / (a - b);
					pos[k] = glm::vec2(tx0 + ((float)i + (i2 - i) * t) / R, tz0 + ((float)j + (j2 - j) * t) / R);
				}
				return k;
			};
			for (int i = 0; i + 1 < nx; i++)
				for (int j = 0; j + 1 < nz; j++)
				{
					int cs = (at(i, j) > 0 ? 1 : 0) | (at(i + 1, j) > 0 ? 2 : 0) | (at(i + 1, j + 1) > 0 ? 4 : 0) | (at(i, j + 1) > 0 ? 8 : 0);
					if (cs == 0 || cs == 15) continue;
					long long e[4] = { crossing(i, j, 0), crossing(i + 1, j, 1), crossing(i, j + 1, 0), crossing(i, j, 1) };	// bottom, right, top, left
					static const int tab[16][4] = { {-1,-1,-1,-1}, {0,3,-1,-1}, {0,1,-1,-1}, {3,1,-1,-1}, {1,2,-1,-1}, {0,3,1,2}, {0,2,-1,-1}, {3,2,-1,-1},
													{2,3,-1,-1}, {0,2,-1,-1}, {0,1,2,3}, {1,2,-1,-1}, {1,3,-1,-1}, {0,1,-1,-1}, {0,3,-1,-1}, {-1,-1,-1,-1} };
					for (int q = 0; q < 4; q += 2)
						if (tab[cs][q] >= 0) { long long a = e[tab[cs][q]], b = e[tab[cs][q + 1]]; adj[a].push_back(b); adj[b].push_back(a); }
				}
			std::vector<Poly> out;
			std::set<long long> seen;
			auto trace = [&](long long start, bool openStart) {
				Poly pl; long long prev = -1, cur = start;
				while (true)
				{
					seen.insert(cur); pl.p.push_back(pos[cur]);
					long long nxt = -1;
					for (auto n : adj[cur]) if (n != prev && !seen.count(n)) { nxt = n; break; }
					if (nxt < 0)
					{
						for (auto n : adj[cur]) if (n == start && n != prev && pl.p.size() > 2) pl.closed = true;
						break;
					}
					prev = cur; cur = nxt;
				}
				return pl;
			};
			for (auto& kv : adj) if (kv.second.size() == 1 && !seen.count(kv.first)) { auto pl = trace(kv.first, true); if (pl.p.size() > 3) out.push_back(pl); }
			for (auto& kv : adj) if (!seen.count(kv.first)) { auto pl = trace(kv.first, false); if (pl.p.size() > 5) out.push_back(pl); }
			for (auto& pl : out)
			{
				int n = (int)pl.p.size();
				for (int pass = 0; pass < 3; pass++)		// smooth
				{
					auto q = pl.p;
					for (int i = 0; i < n; i++)
					{
						if (!pl.closed && (i == 0 || i == n - 1)) continue;
						q[i] = (pl.p[(i + n - 1) % n] + pl.p[i] * 2.0f + pl.p[(i + 1) % n]) * 0.25f;
					}
					pl.p = q;
				}
				// arc length
				pl.s.assign(n, 0.0f);
				for (int i = 1; i < n; i++) pl.s[i] = pl.s[i - 1] + glm::length(pl.p[i] - pl.p[i - 1]);
				pl.L = pl.s[n - 1] + (pl.closed ? glm::length(pl.p[0] - pl.p[n - 1]) : 0.0f);
				float per = std::max(0.5f, st.ribbonPeriod);
				if (pl.closed) { int periods = std::max(1, (int)std::lround(pl.L / per)); pl.k = periods / pl.L; pl.period = (float)periods; }
				else pl.k = 1.0f / per;
			}
			return out;
		}

		// cliff face as a ribbon: continuous UVs from distance across / along the cliff curve
		void ribbonTiles(Gnd* gnd, GroupAction* ga, int& nextId, int tex, const std::vector<glm::ivec2>& tiles)
		{
			auto& st = settings();
			if (centres.empty() || tiles.empty()) return;
			float tx0 = 1e9f, tz0 = 1e9f, tx1 = -1e9f, tz1 = -1e9f;
			for (auto& t : tiles) for (int i = 0; i < 4; i++)
			{
				glm::vec2 c = cornerPos(gnd, t.x, t.y, i);
				tx0 = std::min(tx0, c.x); tx1 = std::max(tx1, c.x); tz0 = std::min(tz0, c.y); tz1 = std::max(tz1, c.y);
			}
			tx0 -= 2; tz0 -= 2; tx1 += 2; tz1 += 2;
			auto polys = contours(st, tx0, tz0, tx1, tz1);
			if (polys.empty()) return;
			// is the inside of the brush the high side?
			float meanH = 0; int cnt = 0;
			for (auto& kv : origHeights) for (int i = 0; i < 4; i++) { meanH += kv.second[i]; cnt++; }
			meanH = cnt ? meanH / cnt : 0.0f;
			bool insideHigh = st.mode == 0 || (st.mode == 2 && st.height < meanH);
			float w = std::max(0.5f, st.edgeWidth);
			float span = st.ribbonBottom - st.ribbonTop, vmid = (st.ribbonTop + st.ribbonBottom) * 0.5f;
			struct Hit { int ci; float d, u; int bi; };
			auto eval = [&](const glm::vec2& p, int forceCi) -> Hit
			{
				Hit h{ -1, 0, 0, 0 }; float best = 1e9f; int bi = 0;
				for (int ci = 0; ci < (int)polys.size(); ci++)
				{
					if (forceCi >= 0 && ci != forceCi) continue;
					for (int i = 0; i < (int)polys[ci].p.size(); i++)
					{
						float d2 = glm::dot(polys[ci].p[i] - p, polys[ci].p[i] - p);
						if (d2 < best) { best = d2; h.ci = ci; bi = i; }
					}
				}
				if (h.ci < 0) return h;
				float sgn = fieldAt(st, p) >= 0 ? 1.0f : -1.0f;
				float d = std::sqrt(best) * sgn;
				d -= w * 0.5f;						// the contour is the foot of the slope; the band is centred on the slope
				h.d = insideHigh ? d : -d;
				h.u = polys[h.ci].k * polys[h.ci].s[bi]; h.bi = bi;
				return h;
			};
			for (auto& t : tiles)
			{
				auto c = gnd->cubes[t.x][t.y];
				if (c->tileUp < 0) continue;
				// one planar mapping per tile: u runs along the cliff curve at the tile centre (tangent + arc length), v is the distance across.
				// A single affine map can never fold or smear within a tile, unlike picking the nearest point on the curve for every corner.
				glm::vec2 centre = (cornerPos(gnd, t.x, t.y, 0) + cornerPos(gnd, t.x, t.y, 3)) * 0.5f;
				Hit hc = eval(centre, -1);
				if (hc.ci < 0) continue;
				auto& pl = polys[hc.ci];
				int n = (int)pl.p.size();
				int bi = hc.bi;
				glm::vec2 tg = pl.p[std::min(n - 1, bi + 2)] - pl.p[std::max(0, bi - 2)];
				if (pl.closed) tg = pl.p[(bi + 2) % n] - pl.p[(bi + n - 2) % n];
				if (glm::length(tg) < 1e-5f) continue;
				tg = glm::normalize(tg);
				float u[4], v[4], vmn = 9, vmx = -9;
				for (int i = 0; i < 4; i++)
				{
					glm::vec2 cp = cornerPos(gnd, t.x, t.y, i);
					Hit h = eval(cp, hc.ci);
					u[i] = pl.k * (pl.s[bi] + glm::dot(cp - centre, tg)) * (st.ribbonFlip ? -1.0f : 1.0f);
					v[i] = vmid - span * h.d / w;
					vmn = std::min(vmn, v[i]); vmx = std::max(vmx, v[i]);
				}
				if (vmx < st.ribbonTop + 0.12f || vmn > st.ribbonBottom + 0.01f) continue;			// no rock in this tile
				float umn = std::min(std::min(u[0], u[1]), std::min(u[2], u[3]));
				// tiles lying mostly beyond the rock band would only smear the edge pixels: leave them as they were
				{
					int over = 0, under = 0;
					for (int i = 0; i < 4; i++) { over += v[i] > 1.12f; under += v[i] < -0.12f; }
					if (over >= 1 || under >= 1) continue;
					if (vmn > st.ribbonBottom - 0.04f || vmx < st.ribbonTop + 0.04f) continue;
				}
				float shift = std::floor(umn);
				for (int i = 0; i < 4; i++) { u[i] -= shift; v[i] = std::clamp(v[i], 0.0f, 1.0f); }
				auto nt = new Gnd::Tile(*gnd->tiles[c->tileUp]);
				nt->textureIndex = (short)tex;
				nt->v1 = glm::vec2(u[0], v[0]); nt->v2 = glm::vec2(u[1], v[1]); nt->v3 = glm::vec2(u[2], v[2]); nt->v4 = glm::vec2(u[3], v[3]);
				ga->addAction(new CubeTileChangeAction(t, c, nextId, c->tileFront, c->tileSide));
				c->tileUp = nextId++;
				ga->addAction(new TileNewAction(nt));
			}
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
			// smart edge texturing: pick straight / corner / diagonal piece per ground tile along the cliff and rotate it
			if (s.profile == 1 && s.smartEdges)
			{
				int pidx[3];
				for (int k = 0; k < 3; k++) pidx[k] = s.pieceTex[k].empty() ? -1 : textureIndexFor(gnd, s.pieceTex[k]);
				if (pidx[0] >= 0 && pidx[1] >= 0 && pidx[2] >= 0)
				{
					const float eps = 0.01f;
					const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
					auto cubeAt = [&](int x, int y) -> Gnd::Cube* { return (x >= 0 && y >= 0 && x < gnd->width && y < gnd->height) ? gnd->cubes[x][y] : nullptr; };
					auto stepAcross = [&](int x, int y, int d) -> bool
					{
						auto c = gnd->cubes[x][y];
						auto n = cubeAt(x + DX[d], y + DY[d]);
						if (!n || n->tileUp < 0) return false;
						if (d == 0) return std::fabs(c->h2 - n->h1) > eps || std::fabs(c->h4 - n->h3) > eps;
						if (d == 1) return std::fabs(c->h1 - n->h2) > eps || std::fabs(c->h3 - n->h4) > eps;
						if (d == 2) return std::fabs(c->h3 - n->h1) > eps || std::fabs(c->h4 - n->h2) > eps;
						return std::fabs(c->h1 - n->h3) > eps || std::fabs(c->h2 - n->h4) > eps;
					};
					for (auto& r : region)
					{
						int x = r.first, y = r.second;
						auto c = gnd->cubes[x][y];
						if (c->tileUp < 0) continue;
						bool st[4]; int count = 0;
						for (int d = 0; d < 4; d++) { st[d] = stepAcross(x, y, d); count += st[d]; }
						int piece = -1; float targetAngle = 0;
						if (count == 0)
						{
							for (int dx = -1; dx <= 1 && piece < 0; dx += 2)
								for (int dy = -1; dy <= 1 && piece < 0; dy += 2)
								{
									auto n = cubeAt(x + dx, y + dy);
									if (!n || n->tileUp < 0) continue;
									int ci = (dy == 1 ? 2 : 0) + (dx == 1 ? 1 : 0), ni = (dy == 1 ? 0 : 2) + (dx == 1 ? 0 : 1);
									if (std::fabs(c->heights[ci] - n->heights[ni]) > eps) { piece = 1; targetAngle = std::atan2((float)dy, (float)dx); }
								}
						}
						else if (count == 2 && ((st[0] || st[1]) && (st[2] || st[3])))
						{
							int dx = st[0] ? 1 : -1, dy = st[2] ? 1 : -1;
							piece = 2; targetAngle = std::atan2((float)dy, (float)dx);
						}
						else
						{
							int d = 0; while (!st[d]) d++;
							piece = 0; targetAngle = std::atan2((float)DY[d], (float)DX[d]);
						}
						if (piece < 0) continue;
						float baseAngle = piece == 0 ? 1.5707963f : 3.9269908f;		// image: straight piece has rock up, the others rock at the lower left
						int quarter = (int)std::lround((targetAngle - baseAngle) / 1.5707963f) + s.pieceRot[piece];
						quarter = ((quarter % 4) + 4) % 4;
						auto q = [&](float u, float v) -> glm::vec2
						{
							float a = -quarter * 1.5707963f, cs = std::round(std::cos(a)), sn = std::round(std::sin(a));
							float px = u - 0.5f, py = v - 0.5f;
							glm::vec2 res(px * cs - py * sn + 0.5f, px * sn + py * cs + 0.5f);
							if (s.pieceFlip[piece]) res.x = 1.0f - res.x;
							return res;
						};
						auto nt = new Gnd::Tile(*gnd->tiles[c->tileUp]);
						nt->textureIndex = (short)pidx[piece];
						nt->v1 = q(0, 0); nt->v2 = q(1, 0); nt->v3 = q(0, 1); nt->v4 = q(1, 1);
						ga->addAction(new CubeTileChangeAction(glm::ivec2(x, y), c, nextId, c->tileFront, c->tileSide));
						c->tileUp = nextId++;
						ga->addAction(new TileNewAction(nt));
					}
				}
			}
			// ribbon texturing for sloped cliffs (continuous UVs along the cliff curve)
			bool ribbonDone = false;
			if (s.profile == 0 && s.ribbon && !s.ribbonTex.empty())
			{
				int rt = textureIndexFor(gnd, s.ribbonTex);
				if (rt >= 0) { { std::vector<glm::ivec2> rtiles; for (auto& r : region) rtiles.push_back(glm::ivec2(r.first, r.second)); ribbonTiles(gnd, ga, nextId, rt, rtiles); } ribbonDone = true; }
			}
			// slope texture: steep ground gets rock
			if (s.profile == 0 && !ribbonDone && s.texSlope && !s.slopeTextures.empty())
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
				centres.clear();
			}
			if (glm::length(c - lastCentre) >= 0.5f)
			{
				int savedMode = s.mode;
				if (io.KeyShift && s.mode < 2) s.mode = 1 - s.mode;
				stamp(gnd, gr, c);
				centres.push_back(c);
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
			ImGui::Separator();
			ImGui::Checkbox("Smart edge textures (ground pieces)", &s.smartEdges);
			if (s.smartEdges)
			{
				static const char* names[3] = { "Straight edge", "Rounded corner", "Diagonal" };
				for (int k = 0; k < 3; k++)
				{
					ImGui::PushID(k);
					ImGui::Text("%s: %s", names[k], s.pieceTex[k].empty() ? "(none)" : s.pieceTex[k].c_str());
					if (ImGui::Button("Use picker texture") && gnd && mv->textureSelected >= 0 && mv->textureSelected < (int)gnd->textures.size())
						s.pieceTex[k] = gnd->textures[mv->textureSelected]->file;
					ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::Combo("##rot", &s.pieceRot[k], "Rot 0\0Rot 90\0Rot 180\0Rot 270\0");
					ImGui::SameLine(); ImGui::Checkbox("Flip", &s.pieceFlip[k]);
					ImGui::PopID();
				}
				ImGui::TextWrapped("Pieces assumed: straight = rock on top, corner and diagonal = rock in the lower left. If a piece comes out turned, adjust its Rot/Flip once.");
			}
		}
		else
		{
			ImGui::Checkbox("Ribbon texture (continuous, follows the curve)", &s.ribbon);
			if (s.ribbon)
			{
				ImGui::Text("Ribbon texture: %s", s.ribbonTex.empty() ? "(none)" : s.ribbonTex.c_str());
				if (ImGui::Button("Use picker texture##ribbon") && gnd && mv->textureSelected >= 0 && mv->textureSelected < (int)gnd->textures.size())
					s.ribbonTex = gnd->textures[mv->textureSelected]->file;
				ImGui::SliderFloat("Cubes per repeat", &s.ribbonPeriod, 1.0f, 16.0f);
				ImGui::SliderFloat("Rock band top (v)", &s.ribbonTop, 0.0f, 0.5f);
				ImGui::SliderFloat("Rock band bottom (v)", &s.ribbonBottom, 0.5f, 1.0f);
				ImGui::Checkbox("Flip direction", &s.ribbonFlip);
				ImGui::TextWrapped("The rock band texture is laid along the cliff curve: u follows the curve, v goes across the slope, so it bends round corners without seams. Used instead of the random slope textures when set.");
			}
			ImGui::Checkbox("Texture steep slopes (random, when no ribbon)", &s.texSlope);
			setEditor("Slope textures (random pick)", s.slopeTextures);
		}
		ImGui::TextWrapped("Pick a texture in the texture picker window, then press Add. Shift while painting flips raise/lower. Alt + wheel changes the radius. The whole stroke is one undo step; cliff faces appear when you release the mouse.");
	}
}
