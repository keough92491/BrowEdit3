#include <glad/gl.h>
#include "SpriteInternal.h"
#include "Text.h"
#include <browedit/BrowEdit.h>
#include <browedit/Map.h>
#include <browedit/MapView.h>
#include <browedit/Node.h>
#include <browedit/actions/Action.h>
#include <browedit/components/Gnd.h>
#include <browedit/components/Gat.h>
#include <browedit/components/SprRenderer.h>
#include <browedit/util/FileIO.h>
#include <browedit/util/ResourceManager.h>
#include <browedit/math/Ray.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>

namespace sprite
{
	const char* categories[] = { "Town Citizens", "Merchants", "Guards", "Adventurers", "Event NPCs", "Custom" };
	const int categoryCount = 6;

	// ------------------------------------------------------------------ instance json
	nlohmann::json Instance::toJson(const Db& db) const
	{
		nlohmann::json j;
		j["id"] = id; j["name"] = name;
		j["kind"] = cfg.kind == 0 ? "player" : cfg.kind == 1 ? "monster" : "npc"; j["mob"] = cfg.mob;
		j["job"] = cfg.job;
		auto je = db.job(cfg.job);
		j["jobId"] = je ? je->id : -1;
		j["gender"] = cfg.gender == Female ? "female" : "male";
		j["hairStyle"] = cfg.hairStyle; j["hairColor"] = cfg.hairColor; j["bodyPalette"] = cfg.bodyPalette;
		j["headgear"] = { cfg.head[0], cfg.head[1], cfg.head[2] };
		j["costumeHeadgear"] = { cfg.costume[0], cfg.costume[1], cfg.costume[2] };
		j["garment"] = cfg.robe; j["weapon"] = cfg.weapon; j["shield"] = cfg.shield;
		j["customAssets"] = { {"wing", cfg.customWing}, {"alignment", cfg.toJson()["wingOff"]} };
		j["action"] = action; j["direction"] = dir; j["speed"] = speed; j["scale"] = scale;
		j["position"] = { pos.x, pos.y, pos.z }; j["elevation"] = elevation;
		j["visible"] = visible; j["locked"] = locked;
		j["editorOnly"] = true;	// never an NPC in the game
		return j;
	}
	Instance Instance::fromJson(const nlohmann::json& j)
	{
		Instance i;
		i.id = j.value("id", 0); i.name = j.value("name", std::string("Sprite"));
		auto& c = i.cfg;
		{ std::string k = j.value("kind", std::string("player")); c.kind = k == "monster" ? 1 : k == "npc" ? 2 : 0; }
		c.mob = j.value("mob", std::string());
		c.job = j.value("job", c.job);
		c.gender = j.value("gender", std::string("male")) == "female" ? Female : Male;
		c.hairStyle = j.value("hairStyle", 1); c.hairColor = j.value("hairColor", 0); c.bodyPalette = j.value("bodyPalette", -1);
		if (j.contains("headgear")) for (int k = 0; k < 3; k++) c.head[k] = j["headgear"][k];
		if (j.contains("costumeHeadgear")) for (int k = 0; k < 3; k++) c.costume[k] = j["costumeHeadgear"][k];
		c.robe = j.value("garment", 0); c.weapon = j.value("weapon", 0); c.shield = j.value("shield", std::string());
		if (j.contains("customAssets"))
		{
			c.customWing = j["customAssets"].value("wing", std::string());
			if (j["customAssets"].contains("alignment"))
				for (int k = 0; k < 8 && k < (int)j["customAssets"]["alignment"].size(); k++) { c.wingOff[k][0] = j["customAssets"]["alignment"][k][0]; c.wingOff[k][1] = j["customAssets"]["alignment"][k][1]; }
		}
		i.action = j.value("action", 0); i.dir = j.value("direction", 0); i.speed = j.value("speed", 1.0f); i.scale = j.value("scale", 1.0f);
		if (j.contains("position")) i.pos = glm::vec3(j["position"][0], j["position"][1], j["position"][2]);
		i.elevation = j.value("elevation", 0.0f);
		i.visible = j.value("visible", true); i.locked = j.value("locked", false);
		return i;
	}

	// ------------------------------------------------------------------ undo
	namespace
	{
		class LayerAction : public Action
		{
			MapLayer before, after;
			std::string title;
			void apply(Map* map, BrowEdit* be, const MapLayer& from)
			{
				auto& L = System::instance().layer(map, be);
				L.items = from.items; L.nextId = std::max(L.nextId, from.nextId);
				for (auto it = L.selected.begin(); it != L.selected.end();)
					it = L.find(*it) ? std::next(it) : L.selected.erase(it);
			}
		public:
			LayerAction(const MapLayer& b, const MapLayer& a, const std::string& t) : before(b), after(a), title(t) {}
			void perform(Map* map, BrowEdit* be) override { apply(map, be, after); }
			void undo(Map* map, BrowEdit* be) override { apply(map, be, before); }
			std::string str() override { return title; }
		};
	}
	void pushLayerAction(Map* map, BrowEdit* be, MapLayer& layer, const MapLayer& before, const std::string& title)
	{
		MapLayer after = layer;
		map->doAction(new LayerAction(before, after, title), be);
	}

	// ------------------------------------------------------------------ system / assets
	System& System::instance() { static System s; return s; }
	System::System() : composer(db, *this), impl(new Impl()) {}
	System::~System() { if (scanThread.joinable()) scanThread.join(); }

	bool System::read(const std::string& path, std::vector<uint8_t>& out)
	{
		try
		{
			std::istream* is = util::FileIO::open(utf8ToRaw(path));
			if (!is) return false;
			is->seekg(0, std::ios::end);
			auto size = is->tellg();
			is->seekg(0, std::ios::beg);
			if (size <= 0) { delete is; return false; }
			out.resize((size_t)size);
			is->read((char*)out.data(), size);
			delete is;
			return true;
		}
		catch (...) { return false; }
	}

	void System::listAll(std::vector<std::string>& paths)
	{
		auto all = util::FileIO::listAllFiles();
		const char* roots[] = { "data\\sprite\\", "data\\palette\\", "data\\imf\\" };
		for (auto& f : all)
		{
			bool keep = false;
			for (auto r : roots)
			{
				size_t n = strlen(r);
				if (f.size() >= n && lower(f.substr(0, n)) == r) { keep = true; break; }
			}
			if (keep) paths.push_back(rawToUtf8(f));
		}
	}

	void System::startScan(BrowEdit* browEdit)
	{
		if (db.scanning) return;
		if (scanThread.joinable()) scanThread.join();
		std::string meta;
		{
			std::ifstream i("data/spritedb.json", std::ios::binary);
			std::stringstream ss; ss << i.rdbuf(); meta = ss.str();
		}
		std::string err;
		if (meta.empty() || !db.loadMeta(meta, err))
			std::cerr << "Sprite: could not load data/spritedb.json: " << err << std::endl;
		composer.clearAll();
		db.ready = false; db.scanning = true;
		scanStarted = true;
		scanThread = std::thread([this]() { try { db.scan(*this); } catch (...) { db.scanning = false; } impl->scanVersion++; });
	}

	void System::ensureReady(BrowEdit* browEdit)
	{
		if (!scanStarted) startScan(browEdit);
		if (!impl->presetsLoaded) impl->loadPresets(*this);
	}

	// ------------------------------------------------------------------ persistence
	MapLayer& System::layer(Map* map, BrowEdit* be)
	{
		auto& L = impl->layers[map];
		if (!L.loaded)
		{
			L.loaded = true;
			std::string n = map->name;
			std::string path = n.find(":") != std::string::npos ? n : be->config.ropath + n;
			if (path.size() > 4 && path.substr(path.size() - 4) == ".rsw") path = path.substr(0, path.size() - 4);
			L.path = path + ".browsprites.json";
			std::ifstream i(L.path, std::ios::binary);
			if (i)
			{
				try
				{
					auto j = nlohmann::json::parse(i);
					for (auto& e : j["sprites"]) { auto in = Instance::fromJson(e); if (in.id <= 0) in.id = L.nextId; L.nextId = std::max(L.nextId, in.id + 1); L.items.push_back(in); }
					L.nextId = std::max(L.nextId, j.value("nextId", 1));
				}
				catch (std::exception& e) { std::cerr << "Sprite: could not read " << L.path << ": " << e.what() << std::endl; }
			}
		}
		return L;
	}

	void System::save(Map* map, BrowEdit* be)
	{
		auto it = impl->layers.find(map);
		if (it == impl->layers.end()) return;
		auto& L = it->second;
		if (L.items.empty() && !std::filesystem::exists(L.path)) return;
		nlohmann::json j;
		j["version"] = 1;
		j["note"] = "BrowEdit editor-only sprite characters. They are not part of the RSW and are not NPCs.";
		j["nextId"] = L.nextId;
		j["sprites"] = nlohmann::json::array();
		for (auto& i : L.items) j["sprites"].push_back(i.toJson(db));
		try
		{
			std::ofstream o(L.path, std::ios::binary);
			o << j.dump(1);
		}
		catch (...) { std::cerr << "Sprite: could not write " << L.path << std::endl; }
	}

	void System::forget(Map* map) { impl->layers.erase(map); }

	// ------------------------------------------------------------------ gl helpers
	unsigned int System::Impl::tex(Frame& f)
	{
		if (f.tex) return f.tex;
		if (f.w <= 0 || f.h <= 0) return 0;
		glGenTextures(1, &f.tex);
		glBindTexture(GL_TEXTURE_2D, f.tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.w, f.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);	// pixel art: nearest
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return f.tex;
	}

	void System::Impl::freeDropped(System& s)
	{
		for (auto& f : s.composer.dropped)
			if (f && f->tex) { glDeleteTextures(1, &f->tex); f->tex = 0; }
		s.composer.dropped.clear();
	}

	static bool sameCfg(const CharacterConfig& a, const CharacterConfig& b)
	{
		return a.kind == b.kind && a.mob == b.mob && a.job == b.job && a.gender == b.gender && a.hairStyle == b.hairStyle && a.hairColor == b.hairColor && a.bodyPalette == b.bodyPalette &&
			a.head[0] == b.head[0] && a.head[1] == b.head[1] && a.head[2] == b.head[2] && a.costume[0] == b.costume[0] && a.costume[1] == b.costume[1] && a.costume[2] == b.costume[2] &&
			a.robe == b.robe && a.weapon == b.weapon && a.shield == b.shield && a.customWing == b.customWing && memcmp(a.wingOff, b.wingOff, sizeof(a.wingOff)) == 0;
	}

	std::shared_ptr<Frame> System::Impl::frameFor(System& s, const Instance& in, int effDir, double time, AnimInfo* infoOut)
	{
		if (!in.rtValid || !sameCfg(in.cfg, in.rtCfg) || in.rtAction != in.action || in.rtDir != effDir)
		{
			in.rtCfg = in.cfg; in.rtKey = in.cfg.key(); in.rtAction = in.action; in.rtDir = effDir; in.rtValid = true;
			in.rtInfo = s.composer.info(in.cfg, in.action, effDir);
			in.rtFrames.assign(in.rtInfo.valid ? in.rtInfo.frames : 0, nullptr);
		}
		if (infoOut) *infoOut = in.rtInfo;
		if (!in.rtInfo.valid) return nullptr;
		int fi = (int)(time * 1000.0 * std::max(0.01f, in.speed) / in.rtInfo.ms) % in.rtInfo.frames;
		if (fi < 0) fi = 0;
		auto& f = in.rtFrames[fi];
		if (!f) f = s.composer.frame_(in.cfg, in.rtKey, in.action, effDir, fi);
		return f;
	}

	void drawFrameQuad(gl::Shader* shader, unsigned int tex, const Frame& f, float ps, const glm::vec4& color)
	{
		float x0 = -f.ox * ps, x1 = (f.w - f.ox) * ps, y1 = f.oy * ps, y0 = -(f.h - f.oy) * ps;
		const float v[4][5] = { {x0, y0, 0, 0, 1}, {x0, y1, 0, 0, 0}, {x1, y1, 0, 1, 0}, {x1, y0, 0, 1, 1} };
		shader->setUniform(SprRenderer::SprShader::Uniforms::color, color);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glVertexAttribPointer(0, 3, GL_FLOAT, false, 5 * sizeof(float), &v[0][0]);
		glVertexAttribPointer(1, 2, GL_FLOAT, false, 5 * sizeof(float), &v[0][3]);
		glDrawArrays(GL_QUADS, 0, 4);
	}

	// ------------------------------------------------------------------ world helpers
	static glm::vec3 toWorld(const glm::vec3& p, Gnd* g) { return glm::vec3(5 * g->width + p.x, -p.y, 10 + 5 * g->height - p.z); }
	static glm::vec3 toRsw(const glm::vec3& w, Gnd* g) { return glm::vec3(w.x - 5 * g->width, -w.y, 10 + 5 * g->height - w.z); }

	static const glm::vec3 NO_HIT(std::numeric_limits<float>().max());

	// height of the walkable surface (GAT) or the ground (GND) below a world x/z position
	static bool surfaceHeight(Map* map, Gnd* gnd, bool useGat, float x, float z, float& y)
	{
		if (useGat)
			if (auto gat = map->rootNode->getComponent<Gat>())
			{
				int cx = (int)floorf(x / 5.0f), cy = (int)floorf((5.0f * gat->height + 10.0f - z) / 5.0f);
				auto r = gat->rayCast(math::Ray(glm::vec3(x, 3000, z), glm::vec3(0, -1, 0)), cx - 1, cy - 2, cx + 2, cy + 3);
				if (r != NO_HIT) { y = r.y; return true; }
			}
		auto r = gnd->rayCast(math::Ray(glm::vec3(x, 3000, z), glm::vec3(0, -1, 0)));
		if (r != NO_HIT) { y = r.y; return true; }
		return false;
	}

	// where the cursor is on the map (GAT surface when 'useGat', else the ground), with grid snapping like objects
	static bool cursorGround(MapView* mv, Gnd* gnd, bool useGat, glm::vec3& out)
	{
		glm::vec3 hit = NO_HIT;
		if (useGat)
			if (auto gat = mv->map->rootNode->getComponent<Gat>()) hit = gat->rayCast(mv->mouseRay);
		if (hit == NO_HIT) hit = gnd->rayCast(mv->mouseRay);
		if (hit == NO_HIT) return false;
		bool snap = mv->snapToGrid;
		if (ImGui::GetIO().KeyShift) snap = !snap;
		if (snap)
		{
			hit.x = glm::round((hit.x - mv->gridOffsetTranslate) / mv->gridSizeTranslate) * mv->gridSizeTranslate + mv->gridOffsetTranslate;
			hit.z = glm::round((hit.z - mv->gridOffsetTranslate) / mv->gridSizeTranslate) * mv->gridSizeTranslate + mv->gridOffsetTranslate;
			float y; if (surfaceHeight(mv->map, gnd, useGat, hit.x, hit.z, y)) hit.y = y;
		}
		out = hit;
		return true;
	}

	// direction of the frame to show: the character's own direction turned by where the camera looks from
	static int viewDir(const glm::mat4& view, int dir)
	{
		glm::vec3 right(view[0][0], view[1][0], view[2][0]);
		float fx = right.z, fz = -right.x;	// camera looking direction on the ground (x east, z south)
		float a = glm::degrees(atan2f(-fx, fz));
		int steps = (int)lroundf(a / 45.0f);
		return (((dir + 4 - steps) % 8) + 8) % 8;
	}

	// ------------------------------------------------------------------ rendering
	void System::render(MapView* mv, BrowEdit* be)
	{
		auto gnd = mv->map->rootNode->getComponent<Gnd>();
		if (!gnd) return;
		auto& L = layer(mv->map, be);
		if (!L.items.empty() && !scanStarted) ensureReady(be);	// sprites from the sidecar file are shown in every mode
		if (!db.ready) return;
		impl->freeDropped(*this);
		impl->ghostValid = false;
		bool ghost = placing && be->editMode == BrowEdit::EditMode::Sprite && mv->hovered;
		if (ghost)
		{
			glm::vec3 hit;
			if (cursorGround(mv, gnd, snapToGat, hit))
			{
				impl->ghostValid = true; impl->ghostWorld = hit;
				draft.pos = toRsw(hit, gnd);
			}
		}
		if (L.items.empty() && !(ghost && impl->ghostValid)) return;

		if (!impl->shader)
		{
			impl->shader = util::ResourceManager<gl::Shader>::load<SprRenderer::SprShader>();
			glGenTextures(1, &impl->whiteTex);
			unsigned char white[4] = { 255,255,255,255 };
			glBindTexture(GL_TEXTURE_2D, impl->whiteTex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
		auto shader = impl->shader;
		const glm::mat4& view = mv->nodeRenderContext.viewMatrix;
		shader->use();
		shader->setUniform(SprRenderer::SprShader::Uniforms::projectionMatrix, mv->nodeRenderContext.projectionMatrix);
		shader->setUniform(SprRenderer::SprShader::Uniforms::cameraMatrix, view);
		shader->setUniform(SprRenderer::SprShader::Uniforms::s_texture, 0);
		glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
		glDisableVertexAttribArray(2); glDisableVertexAttribArray(3); glDisableVertexAttribArray(4);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(1);
		glDisable(GL_CULL_FACE);

		double time = ImGui::GetTime();
		glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
		glm::vec3 fwd = -glm::vec3(view[0][2], view[1][2], view[2][2]);
		const float basePs = 5.0f / 35.0f;

		struct Item { const Instance* in; glm::vec3 world; float depth; bool ghost; };
		std::vector<Item> order;
		for (auto& in : L.items)
			if (in.visible)
			{
				glm::vec3 w = toWorld(in.pos, gnd) + glm::vec3(0, in.elevation, 0);
				order.push_back({ &in, w, glm::dot(w - camPos, fwd), false });
			}
		Instance ghostInst;
		if (ghost && impl->ghostValid)
		{
			ghostInst.cfg = draft.cfg; ghostInst.action = draft.action; ghostInst.dir = draft.dir; ghostInst.speed = draft.speed; ghostInst.scale = draft.scale;
			glm::vec3 w = impl->ghostWorld + glm::vec3(0, draft.elevation, 0);
			order.push_back({ &ghostInst, w, glm::dot(w - camPos, fwd), true });
		}
		std::sort(order.begin(), order.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });

		// shadows first (flat ellipse on the ground)
		if (showShadow)
			for (auto& it : order)
			{
				float r = 6.0f * basePs * it.in->scale * 1.8f;
				glm::vec3 gw = it.world - glm::vec3(0, it.in->elevation, 0);
				shader->setUniform(SprRenderer::SprShader::Uniforms::modelMatrix, glm::translate(glm::mat4(1.0f), gw));
				shader->setUniform(SprRenderer::SprShader::Uniforms::color, glm::vec4(0, 0, 0, it.ghost ? 0.15f : 0.3f));
				glBindTexture(GL_TEXTURE_2D, impl->whiteTex);
				std::vector<float> fan; fan.reserve(24 * 5);
				for (int k = 0; k < 24; k++)
				{
					float a = k / 24.0f * 6.2831853f;
					glm::vec3 off = glm::vec3(view * glm::vec4(cosf(a) * r, 0, sinf(a) * r, 0));
					fan.insert(fan.end(), { off.x, off.y, 0, 0, 0 });
				}
				glVertexAttribPointer(0, 3, GL_FLOAT, false, 5 * sizeof(float), &fan[0]);
				glVertexAttribPointer(1, 2, GL_FLOAT, false, 5 * sizeof(float), &fan[3]);
				glDrawArrays(GL_POLYGON, 0, 24);
			}

		for (auto& it : order)
		{
			const Instance& in = *it.in;
			int eff = viewDir(view, in.dir);
			auto f = impl->frameFor(*this, in, eff, time);
			if (!f || f->w <= 0) continue;
			unsigned int t = impl->tex(*f);
			if (!t) continue;
			shader->setUniform(SprRenderer::SprShader::Uniforms::modelMatrix, glm::translate(glm::mat4(1.0f), it.world));
			float ps = basePs * in.scale;
			bool sel = !it.ghost && L.selected.count(in.id);
			drawFrameQuad(shader, t, *f, ps, it.ghost ? glm::vec4(1, 1, 1, 0.6f) : glm::vec4(1, 1, 1, 1));
			if (sel)
			{
				float x0 = -f->ox * ps, x1 = (f->w - f->ox) * ps, y1 = f->oy * ps, y0 = -(f->h - f->oy) * ps;
				const float v[4][5] = { {x0, y0, 0, 0, 0}, {x0, y1, 0, 0, 0}, {x1, y1, 0, 0, 0}, {x1, y0, 0, 0, 0} };
				shader->setUniform(SprRenderer::SprShader::Uniforms::color, glm::vec4(1.0f, 0.85f, 0.2f, 1));
				glBindTexture(GL_TEXTURE_2D, impl->whiteTex);
				glVertexAttribPointer(0, 3, GL_FLOAT, false, 5 * sizeof(float), &v[0][0]);
				glVertexAttribPointer(1, 2, GL_FLOAT, false, 5 * sizeof(float), &v[0][3]);
				glLineWidth(2.0f);
				glDrawArrays(GL_LINE_LOOP, 0, 4);
				glLineWidth(1.0f);
			}
		}
		glUseProgram(0);
	}

	// ------------------------------------------------------------------ interaction
	static Instance instanceFromDraft(const Draft& d)
	{
		Instance i;
		i.name = d.name; i.cfg = d.cfg; i.action = d.action; i.dir = d.dir; i.speed = d.speed; i.scale = d.scale;
		i.pos = d.pos; i.elevation = d.elevation; i.visible = d.visible; i.locked = d.locked;
		return i;
	}

	static int pick(System& sys, System::Impl& im, MapLayer& L, MapView* mv, double time)
	{
		const glm::mat4& view = mv->nodeRenderContext.viewMatrix;
		auto gnd = mv->map->rootNode->getComponent<Gnd>();
		glm::vec3 right(view[0][0], view[1][0], view[2][0]), up(view[0][1], view[1][1], view[2][1]);
		glm::vec3 fwd = -glm::vec3(view[0][2], view[1][2], view[2][2]);
		const float basePs = 5.0f / 35.0f;
		int best = 0; float bestT = std::numeric_limits<float>::max();
		for (auto& in : L.items)
		{
			if (!in.visible || in.locked) continue;
			auto f = im.frameFor(sys, in, viewDir(view, in.dir), time);
			if (!f || f->w <= 0) continue;
			glm::vec3 w = toWorld(in.pos, gnd) + glm::vec3(0, in.elevation, 0);
			float denom = glm::dot(mv->mouseRay.dir, fwd);
			if (fabsf(denom) < 1e-5f) continue;
			float t = glm::dot(w - mv->mouseRay.origin, fwd) / denom;
			if (t < 0 || t > bestT) continue;
			glm::vec3 p = mv->mouseRay.origin + mv->mouseRay.dir * t - w;
			float ps = basePs * in.scale;
			int px = (int)floorf(glm::dot(p, right) / ps) + f->ox, py = f->oy - (int)ceilf(glm::dot(p, up) / ps);
			bool hit = false;
			for (int dy = -2; dy <= 2 && !hit; dy++)		// a little tolerance so thin parts can be grabbed
				for (int dx = -2; dx <= 2 && !hit; dx++)
				{
					int x = px + dx, y = py + dy;
					if (x >= 0 && y >= 0 && x < f->w && y < f->h && f->rgba[((size_t)y * f->w + x) * 4 + 3] > 20) hit = true;
				}
			if (hit) { best = in.id; bestT = t; }
		}
		return best;
	}

	void System::postRender(MapView* mv, BrowEdit* be)
	{
		if (!db.ready) return;
		auto gnd = mv->map->rootNode->getComponent<Gnd>();
		if (!gnd) return;
		auto& L = layer(mv->map, be);
		auto& io = ImGui::GetIO();
		double time = ImGui::GetTime();
		bool typing = io.WantTextInput;

		if (placing)
		{
			if (!typing && ImGui::IsKeyPressed(GLFW_KEY_ESCAPE)) { placing = false; return; }
			if (mv->hovered && ImGui::IsMouseReleased(1) && io.MouseDragMaxDistanceSqr[1] < 5) { placing = false; return; }
			if (mv->hovered && impl->ghostValid && ImGui::IsMouseClicked(0))
			{
				MapLayer before = L;
				Instance in = instanceFromDraft(draft);
				in.id = L.nextId++;
				in.pos = toRsw(impl->ghostWorld, gnd);
				L.items.push_back(in);
				L.selected = { in.id };
				pushLayerAction(mv->map, be, L, before, "Add sprite " + in.name);
				if (!io.KeyShift) placing = false;
			}
			return;
		}

		if (impl->dragging)
		{
			if (ImGui::IsMouseDown(0))
			{
				glm::vec3 hit;
				if (cursorGround(mv, gnd, snapToGat, hit))
				{
					glm::vec3 d = hit - impl->dragStartWorld;
					if (glm::length(d) > 0.01f) impl->dragMoved = true;
					for (auto& kv : impl->dragOrig)
					{
						auto* in = L.find(kv.first);
						if (!in) continue;
						glm::vec3 w = toWorld(kv.second, gnd) + glm::vec3(d.x, 0, d.z);
						float hy;
						if (surfaceHeight(mv->map, gnd, snapToGat, w.x, w.z, hy)) w.y = hy; else w.y = -kv.second.y;
						in->pos = toRsw(w, gnd);
					}
				}
			}
			else
			{
				impl->dragging = false;
				if (impl->dragMoved)
					pushLayerAction(mv->map, be, L, impl->dragBefore, "Move sprite(s)");
			}
			return;
		}

		if (mv->hovered && ImGui::IsMouseClicked(0))
		{
			int hit = pick(*this, *impl, L, mv, time);
			if (hit)
			{
				if (ImGui::IsMouseDoubleClicked(0))
				{
					L.selected = { hit };
					auto* in = L.find(hit);
					draft.cfg = in->cfg; draft.name = in->name; draft.action = in->action; draft.dir = in->dir; draft.speed = in->speed; draft.scale = in->scale;
					draft.pos = in->pos; draft.elevation = in->elevation; draft.visible = in->visible; draft.locked = in->locked;
					impl->editingId = hit;
				}
				else
				{
					if (io.KeyCtrl) { if (L.selected.count(hit)) L.selected.erase(hit); else L.selected.insert(hit); }
					else if (io.KeyShift) L.selected.insert(hit);
					else if (!L.selected.count(hit)) L.selected = { hit };
					if (L.selected.count(hit) && !io.KeyCtrl)
					{
						glm::vec3 h;
						if (cursorGround(mv, gnd, snapToGat, h))
						{
							impl->dragging = true; impl->dragMoved = false; impl->dragStartWorld = h; impl->dragBefore = L;
							impl->dragOrig.clear();
							for (int id : L.selected) if (auto* in = L.find(id)) if (!in->locked) impl->dragOrig[id] = in->pos;
							if (impl->dragOrig.empty()) impl->dragging = false;
						}
					}
				}
			}
			else if (io.KeyCtrl)
			{	// Ctrl + left click on the ground: add the sprite from the panel right here
				glm::vec3 w;
				if (cursorGround(mv, gnd, snapToGat, w))
				{
					MapLayer before = L;
					Instance in = instanceFromDraft(draft);
					in.id = L.nextId++; in.pos = toRsw(w, gnd);
					L.items.push_back(in); L.selected = { in.id };
					pushLayerAction(mv->map, be, L, before, "Add sprite " + in.name);
				}
			}
			else if (!io.KeyShift)
				L.selected.clear();
		}

		if (!typing && mv->hovered)
		{
			if (ImGui::IsKeyPressed(GLFW_KEY_DELETE) && !L.selected.empty())
			{
				MapLayer before = L;
				L.items.erase(std::remove_if(L.items.begin(), L.items.end(), [&](const Instance& i) { return L.selected.count(i.id) && !i.locked; }), L.items.end());
				L.selected.clear();
				pushLayerAction(mv->map, be, L, before, "Delete sprite(s)");
			}
			if (ImGui::IsKeyPressed(GLFW_KEY_ESCAPE)) L.selected.clear();
		}
	}
}
