#define IMGUI_DEFINE_MATH_OPERATORS
#include <browedit/BrowEdit.h>
#include <browedit/Icons.h>
#include <browedit/Map.h>
#include <browedit/MapView.h>
#include <browedit/Node.h>
#include <browedit/gl/Texture.h>
#include <browedit/gl/FBO.h>
#include <browedit/components/Rsw.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <stb/stb_image_write.h>

#include <set>
#include <map>
#include <queue>
#include <functional>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <browedit/components/Gnd.h>
#include <browedit/components/Gat.h>
#include <browedit/math/HermiteCurve.h>
#include <browedit/math/Ray.h>

namespace
{
	using PosFrame = Rsw::KeyFrameData<std::pair<glm::vec3, glm::vec3>>;
	using TgtFrame = Rsw::KeyFrameData<Rsw::CameraTarget>;

	struct Snap { std::vector<std::vector<Rsw::KeyFrame*>> tracks; float length = 0; };
	std::vector<Snap> undoStack, redoStack;
	struct ClipItem { int track; Rsw::KeyFrame* frame; float rel; };
	std::vector<ClipItem> clipboard;
	std::set<Rsw::KeyFrame*> multiSel;
	Rsw* lastRsw = nullptr;
	float snapStep = 0.0f;
	float playSpeed = 1.0f;

	struct GenParams
	{
		int preset = 0;
		float duration = 40;
		float height = 90;
		float margin = 18;		// percent of the map size that is kept clear at every edge
		float radius = 70;		// orbit / reveal size, percent of the usable area
		bool clockwise = true;
		float keysPerSec = 0.6f;
		bool useViewCenter = false;
	};

	Rsw::KeyFrame* cloneFrame(Rsw::KeyFrame* f)
	{
		if (auto p = dynamic_cast<PosFrame*>(f)) return new PosFrame(*p);
		if (auto t = dynamic_cast<TgtFrame*>(f)) return new TgtFrame(*t);
		return nullptr;
	}

	Snap takeSnap(Rsw* rsw)
	{
		Snap s;
		s.length = rsw->cinematicLength;
		for (auto& t : rsw->cinematicTracks)
		{
			s.tracks.emplace_back();
			for (auto f : t.frames)
				s.tracks.back().push_back(cloneFrame(f));
		}
		return s;
	}

	void restoreSnap(Rsw* rsw, const Snap& s)
	{
		rsw->cinematicLength = s.length;
		for (size_t i = 0; i < s.tracks.size() && i < rsw->cinematicTracks.size(); i++)
		{
			rsw->cinematicTracks[i].frames.clear();
			for (auto f : s.tracks[i])
				rsw->cinematicTracks[i].frames.push_back(cloneFrame(f));
		}
		multiSel.clear();
	}

	void pushUndo(Rsw* rsw)
	{
		undoStack.push_back(takeSnap(rsw));
		if (undoStack.size() > 100)
			undoStack.erase(undoStack.begin());
		redoStack.clear();
	}

	void sortTrack(Rsw::Track& t)
	{
		std::stable_sort(t.frames.begin(), t.frames.end(), [](Rsw::KeyFrame* a, Rsw::KeyFrame* b) { return a->time < b->time; });
	}

	Rsw::KeyFrame* frameBefore(Rsw::Track& tr, float t)
	{
		Rsw::KeyFrame* r = nullptr;
		for (auto f : tr.frames)
			if (f->time <= t && (!r || f->time >= r->time))
				r = f;
		if (!r)
			for (auto f : tr.frames)
				if (!r || f->time < r->time)
					r = f;
		return r;
	}

	// position and tangent of the camera path at time t (clamped before the first / after the last keyframe)
	bool evalPos(Rsw::Track& tr, float t, glm::vec3& pos, glm::vec3& dir)
	{
		std::vector<PosFrame*> fr;
		for (auto f : tr.frames)
			if (auto p = dynamic_cast<PosFrame*>(f))
				fr.push_back(p);
		if (fr.empty())
			return false;
		std::sort(fr.begin(), fr.end(), [](PosFrame* a, PosFrame* b) { return a->time < b->time; });
		if (fr.size() == 1 || t <= fr.front()->time) { pos = fr.front()->data.first; dir = fr.front()->data.second; return true; }
		if (t >= fr.back()->time) { pos = fr.back()->data.first; dir = fr.back()->data.second; return true; }
		for (size_t i = 0; i + 1 < fr.size(); i++)
		{
			if (fr[i]->time <= t && t < fr[i + 1]->time)
			{
				auto a = fr[i], b = fr[i + 1];
				float ft = b->time - a->time;
				if (ft <= 0.00001f) { pos = a->data.first; dir = a->data.second; return true; }
				float mixF = (t - a->time) / ft;
				float len = math::HermiteCurve::getLength(a->data.first, a->data.second, b->data.first, b->data.second);
				pos = math::HermiteCurve::getPointAtDistance(a->data.first, a->data.second, b->data.first, b->data.second, mixF * len);
				glm::vec3 p2 = math::HermiteCurve::getPointAtDistance(a->data.first, a->data.second, b->data.first, b->data.second, glm::min(1.0f, mixF + 0.02f) * len);
				glm::vec3 p1 = math::HermiteCurve::getPointAtDistance(a->data.first, a->data.second, b->data.first, b->data.second, glm::max(0.0f, mixF - 0.02f) * len);
				glm::vec3 d = p2 - p1;
				float l = glm::length(d);
				float speed = glm::max(1.0f, glm::mix(glm::length(a->data.second), glm::length(b->data.second), mixF));
				dir = l > 0.0001f ? d / l * speed : a->data.second;
				return true;
			}
		}
		return false;
	}

	// a new keyframe at time t that starts out with the value the animation has there (instead of zeros)
	Rsw::KeyFrame* makeFrameAt(Rsw* rsw, int trackIndex, float t)
	{
		auto& tr = rsw->cinematicTracks[trackIndex];
		Rsw::KeyFrame* base = frameBefore(tr, t);
		Rsw::KeyFrame* nf = nullptr;
		if (base)
			nf = cloneFrame(base);
		else if (trackIndex == 0)
			nf = new PosFrame(t, std::pair<glm::vec3, glm::vec3>(glm::vec3(0, 50, 0), glm::vec3(0, 0, 100)));
		else
			nf = new TgtFrame(t, Rsw::CameraTarget());
		if (!nf)
			return nullptr;
		nf->time = t;
		if (auto pf = dynamic_cast<PosFrame*>(nf))
		{
			glm::vec3 p, d;
			if (evalPos(rsw->cinematicTracks[0], t, p, d))
			{
				pf->data.first = p;
				pf->data.second = d;
			}
		}
		return nf;
	}

	void editorCamera(MapView* mv, glm::vec3& pos, glm::vec3& center)
	{
		float d = mv->cameraDistance;
		float a = glm::radians(mv->cameraRot.x);
		float b = glm::radians(mv->cameraRot.y);
		center = mv->cameraCenter;
		pos = center + glm::vec3(-d * std::cos(a) * std::sin(b), d * std::sin(a), d * std::cos(a) * std::cos(b));
	}

	void goToShot(Rsw* rsw, MapView* mv, float t)
	{
		glm::vec3 pos, dir;
		if (!evalPos(rsw->cinematicTracks[0], t, pos, dir))
			return;
		glm::vec3 center = pos + glm::normalize(dir + glm::vec3(0, 0.0001f, 0)) * 100.0f;
		if (rsw->cinematicTracks.size() > 1)
			if (auto tf = dynamic_cast<TgtFrame*>(frameBefore(rsw->cinematicTracks[1], t)))
				if (tf->data.lookAt == Rsw::CameraTarget::LookAt::Point)
					center = tf->data.point;
		glm::vec3 o = pos - center;
		float d = glm::clamp(glm::length(o), 5.0f, 4000.0f);
		float a = std::asin(glm::clamp(o.y / glm::max(0.001f, glm::length(o)), 0.0f, 1.0f));
		float b = std::atan2(-o.x, o.z);
		mv->cameraCenter = center;
		mv->cameraDistance = d;
		mv->cameraRot = glm::vec2(glm::degrees(a), glm::degrees(b));
		mv->cinematicPlay = false;
	}

	float groundAt(Gnd* gnd, float x, float z, float fallback)
	{
		glm::vec3 h = gnd->rayCast(math::Ray(glm::vec3(x, 3000.0f, z), glm::vec3(0, -1, 0)), true);
		if (h.x > 1e30f || h.y > 1e30f)
			return fallback;
		return h.y;
	}

	float groundMax(Gnd* gnd, float x, float z, float r, float fallback)
	{
		float m = groundAt(gnd, x, z, fallback);
		for (int i = 0; i < 8; i++)
		{
			float a = i * 3.14159265f / 4.0f;
			m = glm::max(m, groundAt(gnd, x + std::cos(a) * r, z + std::sin(a) * r, fallback));
		}
		return m;
	}

	struct Poly
	{
		std::vector<glm::vec2> pts;
		std::vector<float> cum;
		void build()
		{
			cum.assign(pts.size(), 0.0f);
			for (size_t i = 1; i < pts.size(); i++)
				cum[i] = cum[i - 1] + glm::length(pts[i] - pts[i - 1]);
		}
		glm::vec2 at(float u) const
		{
			if (pts.size() == 1 || cum.back() <= 0.0001f)
				return pts[0];
			float d = glm::clamp(u, 0.0f, 1.0f) * cum.back();
			size_t i = std::upper_bound(cum.begin(), cum.end(), d) - cum.begin();
			if (i == 0) return pts[0];
			if (i >= pts.size()) return pts.back();
			float f = (d - cum[i - 1]) / glm::max(0.0001f, cum[i] - cum[i - 1]);
			return glm::mix(pts[i - 1], pts[i], f);
		}
		void smooth(int window, int passes)
		{
			for (int p = 0; p < passes; p++)
			{
				auto src = pts;
				for (int i = 1; i + 1 < (int)pts.size(); i++)
				{
					glm::vec2 s(0);
					int n = 0;
					for (int k = -window; k <= window; k++)
					{
						int j = i + k;
						if (j < 0 || j >= (int)src.size()) continue;
						s += src[j];
						n++;
					}
					pts[i] = s / (float)n;
				}
			}
		}
	};

	// nearest-neighbour ordering of waypoints, starting at the first one
	std::vector<glm::vec2> orderWaypoints(std::vector<glm::vec2> w)
	{
		std::vector<glm::vec2> out;
		if (w.empty()) return out;
		out.push_back(w[0]);
		w.erase(w.begin());
		while (!w.empty())
		{
			size_t best = 0;
			for (size_t i = 1; i < w.size(); i++)
				if (glm::distance(w[i], out.back()) < glm::distance(w[best], out.back()))
					best = i;
			out.push_back(w[best]);
			w.erase(w.begin() + best);
		}
		return out;
	}

	// Builds the keyframes of an automatic shot. Returns a message when it had to fall back to another preset.
	std::string generateCinematic(Rsw* rsw, MapView* mv, GenParams gp)
	{
		std::string msg;
		auto gnd = mv->map->rootNode->getComponent<Gnd>();
		auto gat = mv->map->rootNode->getComponent<Gat>();
		const float W = gnd->width * 10.0f, D = gnd->height * 10.0f;
		const float mg = glm::clamp(gp.margin, 0.0f, 40.0f) / 100.0f;
		const float x0 = W * mg, x1 = W * (1 - mg), z0 = D * mg, z1 = D * (1 - mg);
		glm::vec2 c(W / 2, D / 2);
		if (gp.useViewCenter)
			c = glm::vec2(mv->cameraCenter.x, mv->cameraCenter.z);
		c = glm::clamp(c, glm::vec2(x0, z0), glm::vec2(x1, z1));
		const float cg = groundAt(gnd, c.x, c.y, 0.0f);
		const float spanX = x1 - x0, spanZ = z1 - z0;
		const float half = 0.5f * glm::min(spanX, spanZ);
		auto clampXZ = [&](glm::vec2 p) { return glm::clamp(p, glm::vec2(x0, z0), glm::vec2(x1, z1)); };

		std::function<glm::vec2(float)> xzAt;
		std::function<float(float)> hAt = [](float) { return 1.0f; };
		bool constTarget = false, loop = false;
		float groundRadius = 35.0f;
		Poly poly;
		const float sgn = gp.clockwise ? 1.0f : -1.0f;
		int preset = gp.preset;

		if (preset == 1)	// city: tour the busiest clusters of objects
		{
			std::map<std::pair<int, int>, std::pair<glm::vec2, int>> bins;
			mv->map->rootNode->traverse([&](Node* n)
			{
				if (!n->getComponent<RswModel>()) return;
				auto o = n->getComponent<RswObject>();
				if (!o) return;
				glm::vec2 p(gnd->width * 5.0f + o->position.x, gnd->height * 5.0f - o->position.z + 10.0f);
				if (p.x < x0 || p.x > x1 || p.y < z0 || p.y > z1) return;
				auto& b = bins[{(int)(p.x / 150), (int)(p.y / 150)}];
				b.first += p;
				b.second++;
			});
			std::vector<std::pair<int, glm::vec2>> ranked;
			for (auto& b : bins)
				ranked.push_back({ b.second.second, b.second.first / (float)b.second.second });
			std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
			if (ranked.size() > 8) ranked.resize(8);
			if (ranked.size() < 3)
			{
				msg = "Not enough objects for the city preset, used the field preset instead.";
				preset = 0;
			}
			else
			{
				std::vector<glm::vec2> w;
				for (auto& r : ranked) w.push_back(r.second);
				poly.pts = orderWaypoints(w);
				poly.build();
				poly.smooth(1, 1);
				poly.build();
				groundRadius = 30.0f;
			}
		}
		if (preset == 2)	// dungeon: walk the walkable cells (breadth first search between waypoints)
		{
			const int gw = gat->width, gh = gat->height;
			auto walkable = [&](int x, int y) {
				if (x < 0 || y < 0 || x >= gw || y >= gh) return false;
				int t = gat->cubes[x][y]->gatType;
				return t == 0 || t == 3 || t == 4 || t == 6;
			};
			auto cellWorld = [&](int x, int y) { return glm::vec2(5.0f * x + 2.5f, 5.0f * gh - 5.0f * y - 2.5f); };
			const float dx0 = W * mg * 0.5f, dx1 = W - dx0, dz0 = D * mg * 0.5f, dz1 = D - dz0;
			std::vector<glm::ivec2> cands;
			for (int y = 4; y < gh; y += 8)
				for (int x = 4; x < gw; x += 8)
				{
					auto w = cellWorld(x, y);
					if (walkable(x, y) && w.x >= dx0 && w.x <= dx1 && w.y >= dz0 && w.y <= dz1)
						cands.push_back({ x, y });
				}
			std::vector<glm::ivec2> chosen;
			if (cands.size() >= 3)
			{
				auto first = std::min_element(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.x < b.x; });
				chosen.push_back(*first);
				while (chosen.size() < 8 && chosen.size() < cands.size())
				{
					float bestD = -1; glm::ivec2 best = cands[0];
					for (auto& cc : cands)
					{
						float md = 1e30f;
						for (auto& ch : chosen) md = glm::min(md, glm::distance(glm::vec2(cc), glm::vec2(ch)));
						if (md > bestD) { bestD = md; best = cc; }
					}
					chosen.push_back(best);
				}
			}
			std::vector<glm::vec2> wpf;
			for (auto& ch : chosen) wpf.push_back(glm::vec2(ch));
			wpf = orderWaypoints(wpf);
			std::vector<glm::vec2> route;
			std::vector<int> prev((size_t)gw * gh);
			for (size_t k = 0; k + 1 < wpf.size(); k++)
			{
				glm::ivec2 a(wpf[k]), b(wpf[k + 1]);
				std::fill(prev.begin(), prev.end(), -2);
				std::queue<glm::ivec2> q;
				q.push(a);
				prev[a.y * gw + a.x] = -1;
				bool found = false;
				while (!q.empty() && !found)
				{
					auto cur = q.front(); q.pop();
					const glm::ivec2 nb[4] = { {1,0},{-1,0},{0,1},{0,-1} };
					for (auto& d : nb)
					{
						glm::ivec2 n = cur + d;
						if (!walkable(n.x, n.y) || prev[n.y * gw + n.x] != -2) continue;
						prev[n.y * gw + n.x] = cur.y * gw + cur.x;
						if (n == b) { found = true; break; }
						q.push(n);
					}
				}
				if (!found) continue;
				std::vector<glm::vec2> leg;
				for (int cur = b.y * gw + b.x; cur != -1; cur = prev[cur])
					leg.push_back(cellWorld(cur % gw, cur / gw));
				std::reverse(leg.begin(), leg.end());
				for (size_t i = 0; i < leg.size(); i += 3)
					route.push_back(leg[i]);
				route.push_back(leg.back());
			}
			if (route.size() < 4)
			{
				msg = "No connected walkable area found for the dungeon preset, used the field preset instead.";
				preset = 0;
			}
			else
			{
				poly.pts = route;
				poly.build();
				poly.smooth(3, 2);
				poly.build();
				groundRadius = 12.0f;
			}
		}

		if (preset == 1 || preset == 2)
		{
			xzAt = [&](float u) { return poly.at(u); };
		}
		else if (preset == 3)	// orbit
		{
			loop = true; constTarget = true;
			float r = glm::max(20.0f, gp.radius / 100.0f * half);
			xzAt = [=](float u) { float th = -1.5707963f + sgn * 6.2831853f * u; return clampXZ(c + r * glm::vec2(std::cos(th), std::sin(th))); };
		}
		else if (preset == 4)	// diagonal sweep
		{
			glm::vec2 a(x0, z0), b(x1, z1);
			if (!gp.clockwise) std::swap(a.x, b.x);
			xzAt = [=](float u) { return glm::mix(a, b, u); };
			hAt = [](float u) { return 1.0f - 0.3f * u; };
		}
		else if (preset == 5)	// reveal: pull in on the focus point
		{
			constTarget = true;
			float farD = glm::max(40.0f, gp.radius / 100.0f * glm::max(spanX, spanZ) * 0.6f);
			float nearD = farD * 0.15f;
			float th = 0.7853982f * sgn;
			xzAt = [=](float u) { float e = u * u * (3 - 2 * u); float d = glm::mix(farD, nearD, e); return clampXZ(c + d * glm::vec2(std::cos(th + sgn * 0.5f * u), std::sin(th + sgn * 0.5f * u))); };
			hAt = [](float u) { float e = u * u * (3 - 2 * u); return glm::mix(3.0f, 1.0f, e); };
		}
		if (preset == 0 || !xzAt)	// field: long gently swaying pass over the middle of the map
		{
			preset = 0;
			bool alongX = spanX >= spanZ;
			glm::vec2 axis = alongX ? glm::vec2(1, 0) : glm::vec2(0, 1);
			glm::vec2 lat(-axis.y, axis.x);
			float L = alongX ? spanX : spanZ, Ls = alongX ? spanZ : spanX;
			xzAt = [=](float u) { return clampXZ(c + axis * ((u - 0.5f) * L * sgn) + lat * (std::sin(u * 6.2831853f) * 0.18f * Ls)); };
			hAt = [](float u) { return 1.4f - 0.5f * u; };
			constTarget = false; loop = false;
		}

		const int N = glm::max(4, (int)std::round(gp.duration * gp.keysPerSec));
		std::vector<glm::vec3> P(N + 1);
		for (int i = 0; i <= N; i++)
		{
			float u = i / (float)N;
			glm::vec2 xz = xzAt(u);
			float g = groundMax(gnd, xz.x, xz.y, groundRadius, cg);
			P[i] = glm::vec3(xz.x, g + gp.height * hAt(u), xz.y);
		}
		for (int i = 1; i < N; i++)
			P[i].y = glm::max(P[i].y * 0.5f + 0.25f * (P[i - 1].y + P[i + 1].y), groundAt(gnd, P[i].x, P[i].z, cg) + gp.height * 0.3f);
		if (loop) P[N] = P[0];

		auto& posTrack = rsw->cinematicTracks[0];
		auto& tgtTrack = rsw->cinematicTracks[1];
		posTrack.frames.clear();
		tgtTrack.frames.clear();
		for (int i = 0; i <= N; i++)
		{
			glm::vec3 m;
			if (loop && i < N) m = (P[(i + 1) % N] - P[(i - 1 + N) % N]) * 0.5f;
			else if (loop) m = (P[1] - P[N - 1]) * 0.5f;
			else if (i == 0) m = P[1] - P[0];
			else if (i == N) m = P[N] - P[N - 1];
			else m = (P[i + 1] - P[i - 1]) * 0.5f;
			posTrack.frames.push_back(new PosFrame(gp.duration * i / (float)N, std::pair<glm::vec3, glm::vec3>(P[i], m)));
		}
		if (constTarget)
		{
			glm::vec3 tp(c.x, cg + 8.0f, c.y);
			tgtTrack.frames.push_back(new TgtFrame(0.0f, Rsw::CameraTarget(tp, 1.0f)));
			tgtTrack.frames.push_back(new TgtFrame(gp.duration, Rsw::CameraTarget(tp, 1.0f)));
		}
		else
		{
			const int M = glm::max(2, (int)std::ceil(gp.duration / 0.5f));
			const float la = glm::clamp(3.0f / gp.duration, 0.02f, 0.2f);
			for (int j = 0; j <= M; j++)
			{
				float u = j / (float)M;
				glm::vec2 q = xzAt(glm::min(1.0f, u + la));
				glm::vec3 tp(q.x, groundAt(gnd, q.x, q.y, cg) + gp.height * 0.2f, q.y);
				if (u + la > 1.0f)		// near the end keep looking along the heading
				{
					glm::vec2 a = xzAt(glm::max(0.0f, u - 0.02f)), b = xzAt(1.0f);
					glm::vec2 dir = b - a;
					if (glm::length(dir) > 0.001f) { dir = glm::normalize(dir) * 60.0f; tp = glm::vec3(b.x + dir.x, tp.y, b.y + dir.y); }
				}
				tgtTrack.frames.push_back(new TgtFrame(gp.duration * u, Rsw::CameraTarget(tp, 1.0f)));
			}
		}
		rsw->cinematicLength = gp.duration;
		return msg;
	}
}

float scale = 100;
float rowHeight = 25;
float leftAreaSize = 200;
float timeSelected = 1;
int selectedKeyFrame = -1;
int selectedTrack = -1;
bool playing = true;


void BrowEdit::showCinematicModeWindow()
{
	if (!activeMapView)
		return;
	auto rsw = activeMapView->map->rootNode->getComponent<Rsw>();

	if (lastRsw != rsw)
	{
		lastRsw = rsw;
		undoStack.clear(); redoStack.clear(); clipboard.clear(); multiSel.clear();
		selectedKeyFrame = -1; selectedTrack = -1;
	}
	if (playing && rsw->cinematicLength > 0)
		timeSelected = (float)std::fmod(timeSelected + ImGui::GetIO().DeltaTime * playSpeed, rsw->cinematicLength);
	while (rsw->cinematicTracks.size() > 0 && rsw->cinematicTracks.size() < 2)
		rsw->cinematicTracks.push_back(Rsw::Track{ rsw->cinematicTracks.size() == 1 ? "Camera Rotation" : "Camera Position" });
	if (rsw->cinematicTracks.size() == 0) // stub data
	{
		Rsw::Track t1{ "Camera Position" };
		Rsw::Track t2{ "Camera Rotation" };
		t1.frames.push_back(new Rsw::KeyFrameData<std::pair<glm::vec3, glm::vec3>>(0, std::pair<glm::vec3, glm::vec3>(glm::vec3(50, 50, 50), glm::vec3(0,0,100))));
		
		t2.frames.push_back(new Rsw::KeyFrameData<Rsw::CameraTarget>(0, Rsw::CameraTarget(glm::vec3(100,10,100), 0.01f)));

		rsw->cinematicTracks.push_back(t1);
		rsw->cinematicTracks.push_back(t2);
	}

	ImGui::Begin("Cinematic Mode");
	if (!activeMapView)
	{
		ImGui::End();
		return;
	}

	ImGui::SetNextItemWidth(150);
	ImGui::DragFloat("##CurrentTime", &timeSelected, 0.1f, 0, 10000.0f);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Animation time");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150);
	ImGui::DragFloat("##Length", &rsw->cinematicLength, 0.1f, 0, 10000.0f);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Animation length");
	ImGui::SameLine();
	if (toolBarButton("Prev", ICON_PREV, "Previous Keyframe", ImVec4(1,1,1,1)))
	{
		if (selectedTrack >= 0 && selectedTrack < rsw->cinematicTracks.size() && selectedKeyFrame > 0)
		{
			selectedKeyFrame--;
			multiSel.clear();
			timeSelected = rsw->cinematicTracks[selectedTrack].frames[selectedKeyFrame]->time;
		}
	}
	ImGui::SameLine();
	if (toolBarButton("PlayPause", playing ? ICON_PAUSE : ICON_PLAY, "Play / Pause", ImVec4(1, 1, 1, 1)))
		playing = !playing;
	ImGui::SameLine();
	if (toolBarButton("Next", ICON_NEXT, "Nexts Keyframe", ImVec4(1, 1, 1, 1)))
	{
		if (selectedTrack >= 0 && selectedTrack < rsw->cinematicTracks.size() && selectedKeyFrame < (int)rsw->cinematicTracks[selectedTrack].frames.size()-1)
		{
			selectedKeyFrame++;
			multiSel.clear();
			timeSelected = rsw->cinematicTracks[selectedTrack].frames[selectedKeyFrame]->time;
		}
	}
	ImGui::SameLine();
	ImGui::Checkbox("##preview", &activeMapView->cinematicPlay);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Preview animation in main viewport");
	ImGui::SameLine();
	if (toolBarButton("Record", ICON_REC, "Record a video", ImVec4(1, 1, 1, 1)))
	{
		ImGui::OpenPopup("RecordVideo");
	}
	if (ImGui::BeginPopupModal("RecordVideo"))
	{
		static float fps = 30;
		static int quality = 25;
		static std::string videoFile = "video.mkv";
		static int resolution[2] = { 1920,1080 };
		ImGui::DragFloat("Fps", &fps, 1, 1, 120);
		ImGui::InputText("Filename", &videoFile);
		ImGui::InputInt2("Resolution", resolution);
		ImGui::InputInt("Quality\n(lower = better)", &quality);
		
		if (ImGui::Button("Record"))
		{
			while(true) {
				std::string path = config.ffmpegPath + " ";
				path += "-y ";
				//path += "-f image2pipe ";
				//path += "-vcodec png ";
				path += "-f rawvideo ";
				path += "-s "+std::to_string(resolution[0]) + "x"+std::to_string(resolution[1]) + " -pix_fmt rgb24 ";
				path += "-r " + std::to_string(fps) + " "; // FPS
				path += "-i - ";
				path += "-pix_fmt yuv420p ";
				path += "-c:v libx265 -crf "+std::to_string(quality) + " ";
				path += "-pix_fmt yuv420p ";
				path += "-nostats ";
				path += videoFile;


				HANDLE hPipeReadStdIn, hPipeWriteStdIn;
				HANDLE hPipeReadStdOut, hPipeWriteStdOut;

				SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES) };
				saAttr.bInheritHandle = TRUE; // Pipe handles are inherited by child process.
				saAttr.lpSecurityDescriptor = NULL;

				if (!CreatePipe(&hPipeReadStdOut, &hPipeWriteStdOut, &saAttr, 0))
				{
					std::cout << "Error creating pipe" << std::endl;
					break;
				}

				if (!CreatePipe(&hPipeReadStdIn, &hPipeWriteStdIn, &saAttr, 0))
				{
					std::cout << "Error creating pipe" << std::endl;
					break;
				}
				SetHandleInformation(hPipeWriteStdIn, HANDLE_FLAG_INHERIT, 0);

				STARTUPINFO si = { sizeof(STARTUPINFO) };
				si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
				si.hStdOutput = hPipeWriteStdOut;
				si.hStdError = hPipeWriteStdOut;
				si.hStdInput = hPipeReadStdIn;

				PROCESS_INFORMATION pi = { 0 };

				BOOL fSuccess = CreateProcess(NULL, (LPSTR)path.c_str(), NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
				if (!fSuccess)
				{
					std::cout << "Could not start ffmpeg" << std::endl;
					CloseHandle(hPipeWriteStdIn);
					CloseHandle(hPipeReadStdIn);
					break;
				}

				std::thread t([&]()
				{
						while (true)
						{
							char buf[1024];
							DWORD read;
							auto success = ::ReadFile(hPipeReadStdOut, buf, 1024, &read, nullptr);
							if (!success)
								break;
							std::cout << std::string(buf, read);
						}
				});
				Sleep(100);

				int totalFrames = (int)(fps * rsw->cinematicLength);

				auto& mv = *activeMapView;
				mv.fbo->resize(resolution[0], resolution[1]);




				for (int i = 0; i < totalFrames; i++)
				{
					float time = i * 1.0f / fps;
					std::cout << "Frame\t" << i <<", "<< time << "s\t";

					mv.cinematicPlay = true;
					timeSelected = time;
					int len = resolution[0] * resolution[1] * 3;
					mv.render(this);
					//char* frameData = mv.fbo->saveToMemoryPng(len);
					char* frameData = mv.fbo->saveToMemoryRaw();
					std::cout << len << " bytes" << std::endl;
					DWORD dwWritten;
					::WriteFile(hPipeWriteStdIn, frameData, len, &dwWritten, NULL);
					delete[] frameData;
				}
				CloseHandle(hPipeWriteStdOut);
				CloseHandle(hPipeReadStdOut);
				CloseHandle(hPipeWriteStdIn);
				CloseHandle(hPipeReadStdIn);
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
				t.join();
				break;
			}
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}


	// ---- extra controls -------------------------------------------------------------------------------
	bool actCopy = false, actPaste = false, actDelete = false, actKeyView = false, actUndo = false, actRedo = false, actGoto = false, actAdd = false;
	float gotoTime = timeSelected;
	{
		auto& io = ImGui::GetIO();
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput)
		{
			if (io.KeyCtrl && ImGui::IsKeyPressed(GLFW_KEY_C)) actCopy = true;
			if (io.KeyCtrl && (ImGui::IsKeyPressed(GLFW_KEY_V) || ImGui::IsKeyPressed(GLFW_KEY_D))) actPaste = true;
			if (ImGui::IsKeyPressed(GLFW_KEY_DELETE)) actDelete = true;
		}
		if (ImGui::Button("Key from view")) actKeyView = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fly the editor camera where you want the shot, then press this:\nit adds a camera position + target keyframe at the playhead");
		ImGui::SameLine();
		if (ImGui::Button("Add key")) actAdd = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add keyframes at the playhead that start with the current animated values\n(or double-click a track; new keys copy the value at that time, not zeros)");
		ImGui::SameLine();
		if (ImGui::Button("View shot")) { actGoto = true; gotoTime = timeSelected; }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move the editor camera to the shot at the playhead\n(double-click a keyframe does the same)");
		ImGui::SameLine();
		if (ImGui::Button("Copy")) actCopy = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy selected keyframes (Ctrl+C). Ctrl+click selects several");
		ImGui::SameLine();
		if (ImGui::Button("Paste")) actPaste = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paste at the playhead (Ctrl+V / Ctrl+D)");
		ImGui::SameLine();
		if (ImGui::Button("Delete")) actDelete = true;
		ImGui::SameLine();
		if (ImGui::Button("Undo")) actUndo = true;
		ImGui::SameLine();
		if (ImGui::Button("Redo")) actRedo = true;
		ImGui::SameLine();
		if (ImGui::Button("Generate...")) ImGui::OpenPopup("GenerateCinematic");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatic cinematic based on the map (field, city, dungeon, orbit, ...)");

		ImGui::SetNextItemWidth(80);
		ImGui::DragFloat("Play speed", &playSpeed, 0.01f, 0.1f, 4.0f, "%.2fx");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70);
		static int snapIdx = 0;
		static const float snapValues[] = { 0.0f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f };
		if (ImGui::Combo("Snap", &snapIdx, "Off\0" "0.05 s\0" "0.1 s\0" "0.25 s\0" "0.5 s\0" "1 s\0"))
			snapStep = snapValues[snapIdx];
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120);
		ImGui::SliderFloat("Zoom", &scale, 20.0f, 400.0f, "%.0f");
	}

	if (ImGui::BeginPopupModal("GenerateCinematic", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		static GenParams gp;
		static int lastPreset = 0;
		static std::string resultMsg;
		if (ImGui::Combo("Preset", &gp.preset, "Field (wide flyover)\0City (tour of the busiest areas)\0Dungeon (walk the corridors)\0Orbit around the centre\0Diagonal sweep\0Reveal (pull in on a point)\0"))
		{
			static const float defH[] = { 90, 60, 22, 80, 50, 60 };
			static const float defD[] = { 40, 45, 45, 30, 25, 20 };
			gp.height = defH[gp.preset];
			gp.duration = defD[gp.preset];
		}
		ImGui::DragFloat("Duration (s)", &gp.duration, 0.5f, 4.0f, 600.0f);
		ImGui::DragFloat("Camera height", &gp.height, 1.0f, 5.0f, 500.0f);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height above the ground below the camera.\nRaise it if the camera clips through tall buildings.");
		ImGui::SliderFloat("Edge margin (%)", &gp.margin, 0.0f, 40.0f, "%.0f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stays this far away from the map edges and corners, they are rarely worth showing");
		if (gp.preset == 3 || gp.preset == 5)
			ImGui::SliderFloat("Size (%)", &gp.radius, 20.0f, 100.0f, "%.0f");
		ImGui::SliderFloat("Keys per second", &gp.keysPerSec, 0.2f, 2.0f, "%.2f");
		ImGui::Checkbox("Clockwise / forward", &gp.clockwise);
		ImGui::Checkbox("Use the editor view centre as focus", &gp.useViewCenter);
		if (!resultMsg.empty())
			ImGui::TextWrapped("%s", resultMsg.c_str());
		if (ImGui::Button("Generate"))
		{
			pushUndo(rsw);
			resultMsg = generateCinematic(rsw, activeMapView, gp);
			timeSelected = 0;
			selectedKeyFrame = -1; selectedTrack = -1;
			multiSel.clear();
			if (resultMsg.empty())
				ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Close"))
		{
			resultMsg.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// ---- timeline -------------------------------------------------------------------------------------
	if (rsw->cinematicLength > 0 && ImGui::BeginChild("KeyframeEditor", ImVec2(-1, 200), true, ImGuiWindowFlags_AlwaysHorizontalScrollbar))
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImGuiIO& IO = ImGui::GetIO();
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		auto window = ImGui::GetCurrentWindow();

		ImRect bb(window->DC.CursorPos, window->DC.CursorPos + ImVec2(leftAreaSize + scale * rsw->cinematicLength, 100));
		ImGui::ItemSize(bb);
		if (!ImGui::ItemAdd(bb, NULL))
		{
			ImGui::EndChild();
			ImGui::End();
			return;
		}
		ImVec2 cursorPos = ImGui::GetCursorScreenPos();
		ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_PopupBg, 1), true, style.FrameRounding);

		Rsw::KeyFrame* primaryPtr = nullptr;
		if (selectedTrack >= 0 && selectedTrack < (int)rsw->cinematicTracks.size() && selectedKeyFrame >= 0 && selectedKeyFrame < (int)rsw->cinematicTracks[selectedTrack].frames.size())
			primaryPtr = rsw->cinematicTracks[selectedTrack].frames[selectedKeyFrame];

		int createTrack = -1;
		float createT = 0;
		bool overKeyframe = false;
		bool needSort = false;
		float gotoFrameTime = -1;

		int trackIndex = 0;
		for (auto& track : rsw->cinematicTracks)
		{
			ImGui::RenderText(bb.Min + ImVec2(10, rowHeight + rowHeight * trackIndex + 2), track.name.c_str()); //todo: center
			ImGui::RenderFrame(bb.Min + ImVec2(leftAreaSize, rowHeight + rowHeight * trackIndex  +1), bb.Min + ImVec2(leftAreaSize + scale * rsw->cinematicLength, rowHeight + rowHeight * trackIndex + rowHeight-2), ImGui::GetColorU32(ImGuiCol_FrameBg, 1), true, style.FrameRounding);
			if (trackIndex < 2 && ImGui::IsMouseHoveringRect(bb.Min + ImVec2(leftAreaSize, rowHeight + rowHeight * trackIndex + 1), bb.Min + ImVec2(leftAreaSize + scale * rsw->cinematicLength, rowHeight + rowHeight * trackIndex + rowHeight - 2)) && ImGui::IsMouseDoubleClicked(0))
			{
				createTrack = trackIndex;
				createT = glm::clamp((ImGui::GetMousePos().x - (bb.Min.x + leftAreaSize)) / scale, 0.0f, rsw->cinematicLength);
			}
			trackIndex++;
		}

		for (int i = 0; i < rsw->cinematicLength; i++)
		{
			ImGui::RenderText(bb.Min + ImVec2(leftAreaSize + scale * i + 4, 0), std::to_string(i).c_str());
			for (int ii = 0; ii < 10; ii++)
			{
				window->DrawList->AddLine(bb.Min + ImVec2(leftAreaSize + scale * i + scale / 10.0f * ii, rowHeight - 5), bb.Min + ImVec2(leftAreaSize + scale * i + scale / 10.0f * ii, rowHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled, 1.0f));
				window->DrawList->AddLine(bb.Min + ImVec2(leftAreaSize + scale * i + scale / 10.0f * ii, rowHeight), bb.Min + ImVec2(leftAreaSize + scale * i + scale / 10.0f * ii, rowHeight * (rsw->cinematicTracks.size() + 1)), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f));
			}
			window->DrawList->AddLine(bb.Min + ImVec2(leftAreaSize + scale * i, 0), bb.Min + ImVec2(leftAreaSize + scale * i, rowHeight * (rsw->cinematicTracks.size() + 1)), ImGui::GetColorU32(ImGuiCol_Text, 1));

		}

		static std::map<Rsw::KeyFrame*, float> dragStart;
		static float dragAcc = 0;
		static bool dragUndoDone = false;
		trackIndex = 0;
		for (auto& track : rsw->cinematicTracks)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_Button, 10));
			for (int keyframeIndex = 0; keyframeIndex < (int)track.frames.size(); keyframeIndex++)
			{
				auto keyframe = track.frames[keyframeIndex];
				ImGui::PushID(keyframe);
				bool selected = keyframe == primaryPtr || multiSel.count(keyframe) > 0;
				if (selected)
				{
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_PlotHistogram, 10));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetColorU32(ImGuiCol_PlotHistogram, 10));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetColorU32(ImGuiCol_PlotHistogram, 10));
				}
				ImGui::SetCursorScreenPos(bb.Min + ImVec2(leftAreaSize + scale * keyframe->time - 5, rowHeight + rowHeight * trackIndex));
				ImGui::Button("##", ImVec2(10, rowHeight));
				if (ImGui::IsItemHovered())
					overKeyframe = true;
				if (ImGui::IsItemActivated())
				{
					if (IO.KeyCtrl)
					{
						if (multiSel.count(keyframe)) multiSel.erase(keyframe);
						else
						{
							if (primaryPtr) multiSel.insert(primaryPtr);
							multiSel.insert(keyframe);
						}
					}
					else if (!multiSel.count(keyframe) && keyframe != primaryPtr)
					{
						multiSel.clear();
						multiSel.insert(keyframe);
					}
					selectedTrack = trackIndex;
					selectedKeyFrame = keyframeIndex;
					primaryPtr = keyframe;
					dragStart.clear();
					for (auto& t2 : rsw->cinematicTracks)
						for (auto f : t2.frames)
							if (multiSel.count(f) || f == keyframe)
								dragStart[f] = f->time;
					dragAcc = 0;
					dragUndoDone = false;
				}
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
					gotoFrameTime = keyframe->time;
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !multiSel.count(keyframe) && keyframe != primaryPtr)
				{
					multiSel.clear();
					multiSel.insert(keyframe);
					selectedTrack = trackIndex;
					selectedKeyFrame = keyframeIndex;
					primaryPtr = keyframe;
				}
				if (ImGui::BeginPopupContextItem("KeyFramePopup"))
				{
					if (ImGui::MenuItem("Go to this shot")) gotoFrameTime = keyframe->time;
					if (ImGui::MenuItem("Copy", "Ctrl+C")) actCopy = true;
					if (ImGui::MenuItem("Duplicate at playhead", "Ctrl+D")) { actCopy = true; actPaste = true; }
					if (ImGui::MenuItem("Delete", "Del")) actDelete = true;
					ImGui::EndPopup();
				}
				if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !dragStart.empty())
				{
					dragAcc += ImGui::GetIO().MouseDelta.x / scale;
					if (ImGui::GetIO().MouseDelta.x != 0 && !dragUndoDone)
					{
						// the snapshot is taken from the positions before this drag started
						std::map<Rsw::KeyFrame*, float> cur;
						for (auto& kv : dragStart) { cur[kv.first] = kv.first->time; kv.first->time = kv.second; }
						pushUndo(rsw);
						for (auto& kv : cur) kv.first->time = kv.second;
						dragUndoDone = true;
					}
					for (auto& kv : dragStart)
					{
						float nt = kv.second + dragAcc;
						if (snapStep > 0)
							nt = std::round(nt / snapStep) * snapStep;
						kv.first->time = glm::clamp(nt, 0.0f, rsw->cinematicLength);
					}
					needSort = true;
				}
				if (selected)
					ImGui::PopStyleColor(3);
				ImGui::PopID();
			}
			ImGui::PopStyleColor();
			trackIndex++;
		}

		if (needSort)
		{
			for (auto& t : rsw->cinematicTracks)
				sortTrack(t);
			if (primaryPtr)
				for (int ti = 0; ti < (int)rsw->cinematicTracks.size(); ti++)
					for (int fi = 0; fi < (int)rsw->cinematicTracks[ti].frames.size(); fi++)
						if (rsw->cinematicTracks[ti].frames[fi] == primaryPtr)
						{
							selectedTrack = ti;
							selectedKeyFrame = fi;
						}
		}

		if (gotoFrameTime >= 0)
		{
			actGoto = true;
			gotoTime = gotoFrameTime;
			timeSelected = gotoFrameTime;
		}

		if (createTrack >= 0 && !overKeyframe)
		{
			pushUndo(rsw);
			auto keyframe = makeFrameAt(rsw, createTrack, createT);
			if (keyframe)
			{
				rsw->cinematicTracks[createTrack].frames.push_back(keyframe);
				sortTrack(rsw->cinematicTracks[createTrack]);
				multiSel.clear();
				multiSel.insert(keyframe);
				selectedTrack = createTrack;
				for (int i = 0; i < (int)rsw->cinematicTracks[createTrack].frames.size(); i++)
					if (rsw->cinematicTracks[createTrack].frames[i] == keyframe)
						selectedKeyFrame = i;
			}
		}

		window->DrawList->AddLine(bb.Min + ImVec2(leftAreaSize + scale * timeSelected, 0), bb.Min + ImVec2(leftAreaSize + scale * timeSelected, rowHeight * (rsw->cinematicTracks.size() + 1)), ImGui::GetColorU32(ImGuiCol_Text, 1));
		ImGui::SetCursorScreenPos(bb.Min + ImVec2(leftAreaSize, 0));
		ImGui::InvisibleButton("##", ImVec2(scale * rsw->cinematicLength, rowHeight));
		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			timeSelected = (ImGui::GetMousePos().x - (bb.Min.x+leftAreaSize)) / scale;
			if (timeSelected > rsw->cinematicLength)
				timeSelected = rsw->cinematicLength;
			if (timeSelected < 0)
				timeSelected = 0;
		}


		ImGui::SetCursorScreenPos(cursorPos);


		ImGui::EndChild();
	}

	// ---- actions (buttons, shortcuts and context menus all end up here) -----------------------------
	auto selection = [&]()
	{
		std::vector<std::pair<int, Rsw::KeyFrame*>> out;
		for (int ti = 0; ti < (int)rsw->cinematicTracks.size(); ti++)
			for (int fi = 0; fi < (int)rsw->cinematicTracks[ti].frames.size(); fi++)
			{
				auto f = rsw->cinematicTracks[ti].frames[fi];
				if (multiSel.count(f) || (ti == selectedTrack && fi == selectedKeyFrame))
					out.push_back({ ti, f });
			}
		return out;
	};
	if (actUndo && !undoStack.empty())
	{
		redoStack.push_back(takeSnap(rsw));
		restoreSnap(rsw, undoStack.back());
		undoStack.pop_back();
		selectedKeyFrame = -1; selectedTrack = -1;
	}
	if (actRedo && !redoStack.empty())
	{
		undoStack.push_back(takeSnap(rsw));
		restoreSnap(rsw, redoStack.back());
		redoStack.pop_back();
		selectedKeyFrame = -1; selectedTrack = -1;
	}
	if (actCopy)
	{
		auto sel = selection();
		if (!sel.empty())
		{
			clipboard.clear();
			float base = sel[0].second->time;
			for (auto& s2 : sel) base = glm::min(base, s2.second->time);
			for (auto& s2 : sel)
				clipboard.push_back({ s2.first, cloneFrame(s2.second), s2.second->time - base });
		}
	}
	if (actPaste && !clipboard.empty())
	{
		pushUndo(rsw);
		multiSel.clear();
		float maxT = rsw->cinematicLength;
		for (auto& item : clipboard)
		{
			auto nf = cloneFrame(item.frame);
			if (!nf || item.track >= (int)rsw->cinematicTracks.size()) continue;
			nf->time = timeSelected + item.rel;
			maxT = glm::max(maxT, nf->time);
			rsw->cinematicTracks[item.track].frames.push_back(nf);
			multiSel.insert(nf);
		}
		for (auto& t : rsw->cinematicTracks) sortTrack(t);
		rsw->cinematicLength = std::ceil(maxT);
		selectedKeyFrame = -1; selectedTrack = -1;
	}
	if (actDelete)
	{
		auto sel = selection();
		if (!sel.empty())
		{
			pushUndo(rsw);
			for (int ti = 0; ti < (int)rsw->cinematicTracks.size(); ti++)
			{
				auto& fr = rsw->cinematicTracks[ti].frames;
				std::vector<Rsw::KeyFrame*> keep;
				for (auto f : fr)
				{
					bool del = false;
					for (auto& s2 : sel) if (s2.second == f) del = true;
					if (!del) keep.push_back(f);
				}
				if (!keep.empty())		// a track keeps at least one keyframe
					fr = keep;
			}
			multiSel.clear();
			selectedKeyFrame = -1; selectedTrack = -1;
		}
	}
	if (actAdd)
	{
		pushUndo(rsw);
		for (int ti = 0; ti < 2 && ti < (int)rsw->cinematicTracks.size(); ti++)
			if (auto nf = makeFrameAt(rsw, ti, timeSelected))
			{
				rsw->cinematicTracks[ti].frames.push_back(nf);
				sortTrack(rsw->cinematicTracks[ti]);
			}
	}
	if (actKeyView)
	{
		pushUndo(rsw);
		glm::vec3 camPos, center;
		editorCamera(activeMapView, camPos, center);
		auto findNear = [&](int ti) -> Rsw::KeyFrame* {
			for (auto f : rsw->cinematicTracks[ti].frames)
				if (std::abs(f->time - timeSelected) < 0.05f) return f;
			return nullptr;
		};
		// position
		auto pf = dynamic_cast<PosFrame*>(findNear(0));
		if (!pf)
		{
			pf = dynamic_cast<PosFrame*>(makeFrameAt(rsw, 0, timeSelected));
			if (pf) { rsw->cinematicTracks[0].frames.push_back(pf); sortTrack(rsw->cinematicTracks[0]); }
		}
		if (pf)
		{
			pf->data.first = camPos;
			if (glm::length(pf->data.second) < 1.0f)
				pf->data.second = glm::normalize(center - camPos + glm::vec3(0, 0.001f, 0)) * 100.0f;
		}
		// target
		auto tf = dynamic_cast<TgtFrame*>(findNear(1));
		if (!tf)
		{
			tf = dynamic_cast<TgtFrame*>(makeFrameAt(rsw, 1, timeSelected));
			if (tf) { rsw->cinematicTracks[1].frames.push_back(tf); sortTrack(rsw->cinematicTracks[1]); }
		}
		if (tf)
		{
			tf->data.lookAt = Rsw::CameraTarget::LookAt::Point;
			tf->data.point = center;
		}
	}
	if (actGoto)
		goToShot(rsw, activeMapView, gotoTime);

	ImGui::End();


	if (ImGui::Begin("Cinematic Mode Properties"))
	{
		bool selected = selectedTrack >= 0 && selectedTrack < rsw->cinematicTracks.size() && selectedKeyFrame >= 0 && selectedKeyFrame < rsw->cinematicTracks[selectedTrack].frames.size();
		if (selected)
		{
			auto keyFrame = rsw->cinematicTracks[selectedTrack].frames[selectedKeyFrame];

			bool wasActive = ImGui::IsAnyItemActive();
			Snap before = takeSnap(rsw);
			keyFrame->buildEditor();
			if (!wasActive && ImGui::IsAnyItemActive())
			{
				undoStack.push_back(before);
				redoStack.clear();
			}
		}
	}
	ImGui::End();
}
