#include <glad/gl.h>
#include "AdvTex.h"
#include <browedit/BrowEdit.h>
#include <browedit/Map.h>
#include <browedit/MapView.h>
#include <browedit/Node.h>
#include <browedit/actions/Action.h>
#include <browedit/components/Gnd.h>
#include <browedit/gl/FBO.h>
#include <browedit/gl/Texture.h>
#include <browedit/math/Ray.h>
#include <browedit/shaders/SimpleShader.h>
#include <browedit/util/FileIO.h>
#include <browedit/util/ResourceManager.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace advtex
{
	static const glm::vec3 NO_HIT(std::numeric_limits<float>().max());

	// ------------------------------------------------------------------ state of the current stroke
	static bool stroking = false, strokeErase = false, haveLast = false;
	static glm::vec2 lastPos;
	static std::map<int, Masks> strokeBefore;
	static float brushRadius = 1.5f;		// cells
	static float brushOpacity = 1.0f, brushHardness = 0.5f;
	static bool eraseMode = false;
	static std::string statusLine;

	static bool captureOrig(Gnd* gnd, int x, int y, Cell& c)
	{
		if (x < 0 || y < 0 || x >= gnd->width || y >= gnd->height) return false;
		auto cube = gnd->cubes[x][y];
		if (cube->tileUp < 0) return false;
		auto t = gnd->tiles[cube->tileUp];
		if (t->textureIndex < 0 || t->textureIndex >= (int)gnd->textures.size()) return false;
		c.orig.valid = true; c.orig.file = gnd->textures[t->textureIndex]->file;
		c.orig.uv[0] = t->v1.x; c.orig.uv[1] = t->v1.y; c.orig.uv[2] = t->v2.x; c.orig.uv[3] = t->v2.y;
		c.orig.uv[4] = t->v3.x; c.orig.uv[5] = t->v3.y; c.orig.uv[6] = t->v4.x; c.orig.uv[7] = t->v4.y;
		return true;
	}

	// ------------------------------------------------------------------ undo / redo (paint data only; files are written by the next bake)
	class PaintAction : public Action
	{
		std::map<int, Masks> before, after;
		std::string title;
		void apply(Map* map, BrowEdit* be, const std::map<int, Masks>& data)
		{
			auto& P = project(map, be);
			auto gnd = map->rootNode->getComponent<Gnd>();
			for (auto& kv : data)
			{
				auto it = P.cells.find(kv.first);
				if (it == P.cells.end())
				{
					Cell c;
					if (!captureOrig(gnd, keyX(kv.first), keyY(kv.first), c)) continue;
					it = P.cells.emplace(kv.first, c).first;
				}
				it->second.masks = kv.second;
				it->second.dirty = true;
			}
			P.pending = true;
		}
	public:
		PaintAction(const std::map<int, Masks>& b, const std::map<int, Masks>& a, const std::string& t) : before(b), after(a), title(t) {}
		void perform(Map* map, BrowEdit* be) override { apply(map, be, after); }
		void undo(Map* map, BrowEdit* be) override { apply(map, be, before); }
		std::string str() override { return title; }
	};

	// ------------------------------------------------------------------ painting
	static void stamp(Map* map, Project& P, Gnd* gnd, glm::vec2 p, float R, bool erase, int layer)
	{
		int mMin = (int)floorf((p.y - R) / 10.0f), mMax = (int)floorf((p.y + R) / 10.0f);
		int x0 = (int)floorf((p.x - R) / 10.0f), x1 = (int)floorf((p.x + R) / 10.0f);
		for (int cy = gnd->height - mMax; cy <= gnd->height - mMin; cy++)
			for (int cx = x0; cx <= x1; cx++)
			{
				if (cx < 0 || cy < 0 || cx >= gnd->width || cy >= gnd->height) continue;
				if (gnd->cubes[cx][cy]->tileUp < 0) continue;
				int k = key(cx, cy);
				auto it = P.cells.find(k);
				if (erase && (it == P.cells.end() || !it->second.masks.count(layer))) continue;
				bool created = false;
				if (it == P.cells.end())
				{
					Cell c;
					if (!captureOrig(gnd, cx, cy, c)) continue;
					it = P.cells.emplace(k, c).first; created = true;
				}
				Cell& c = it->second;
				if (!c.orig.valid && !captureOrig(gnd, cx, cy, c)) { if (created) P.cells.erase(it); continue; }
				if (!strokeBefore.count(k)) strokeBefore[k] = c.masks;
				auto& m = c.masks[layer];
				if (m.empty()) m.assign(MASK * MASK, 0);
				bool changed = false;
				for (int j = 0; j < MASK; j++)
					for (int i = 0; i < MASK; i++)
					{
						float wx = 10.0f * cx + 10.0f * (i + 0.5f) / MASK, wz = 10.0f * (gnd->height - cy) + 10.0f * (j + 0.5f) / MASK;
						float d = sqrtf((wx - p.x) * (wx - p.x) + (wz - p.y) * (wz - p.y)) / R;
						if (d >= 1.0f) continue;
						float w = d <= brushHardness ? 1.0f : 1.0f - (d - brushHardness) / (1.0f - brushHardness + 1e-4f);
						w = std::clamp(w, 0.0f, 1.0f) * brushOpacity;
						uint8_t& v = m[j * MASK + i];
						uint8_t nv = erase ? (uint8_t)lroundf(v * (1.0f - w)) : (uint8_t)std::max<int>(v, (int)lroundf(255.0f * w));
						if (nv != v) { v = nv; changed = true; }
					}
				if (changed) { c.dirty = true; P.pending = true; }
			}
	}

	static void markLayerDirty(Project& P, int layer)
	{
		for (auto& kv : P.cells) if (kv.second.masks.count(layer)) kv.second.dirty = true;
		P.pending = true;
	}

	// ------------------------------------------------------------------ viewport
	static void drawRing(MapView* mv, Gnd* gnd, glm::vec2 p, float R, glm::vec4 color)
	{
		std::vector<float> v;
		for (int i = 0; i < 64; i++)
		{
			float a = i / 64.0f * 6.2831853f, x = p.x + cosf(a) * R, z = p.y + sinf(a) * R;
			auto r = gnd->rayCast(math::Ray(glm::vec3(x, 3000, z), glm::vec3(0, -1, 0)));
			float y = r != NO_HIT ? r.y : 0;
			v.insert(v.end(), { x, y + 0.5f, z, 0, 0 });
		}
		mv->simpleShader->use();
		mv->simpleShader->setUniform(SimpleShader::Uniforms::projectionMatrix, mv->nodeRenderContext.projectionMatrix);
		mv->simpleShader->setUniform(SimpleShader::Uniforms::viewMatrix, mv->nodeRenderContext.viewMatrix);
		mv->simpleShader->setUniform(SimpleShader::Uniforms::modelMatrix, glm::mat4(1.0f));
		mv->simpleShader->setUniform(SimpleShader::Uniforms::textureFac, 0.0f);
		mv->simpleShader->setUniform(SimpleShader::Uniforms::color, color);
		mv->fbo->bind();
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDisable(GL_TEXTURE_2D);
		glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
		glDisableVertexAttribArray(2); glDisableVertexAttribArray(3); glDisableVertexAttribArray(4);
		glVertexAttribPointer(0, 3, GL_FLOAT, false, 5 * sizeof(float), &v[0]);
		glVertexAttribPointer(1, 2, GL_FLOAT, false, 5 * sizeof(float), &v[3]);
		glLineWidth(2);
		glDrawArrays(GL_LINE_LOOP, 0, 64);
		glLineWidth(1);
		mv->fbo->unbind();
	}

	void postRender(MapView* mv, BrowEdit* be)
	{
		auto gnd = mv->map->rootNode->getComponent<Gnd>();
		if (!gnd) return;
		Project& P = project(mv->map, be);
		auto& io = ImGui::GetIO();
		bool layerOk = P.selLayer >= 0 && P.selLayer < (int)P.layers.size();
		glm::vec3 hit = mv->hovered || stroking ? gnd->rayCast(mv->mouseRay) : NO_HIT;
		bool ok = hit != NO_HIT;
		float R = brushRadius * 10.0f;

		if (!stroking && mv->hovered && ok && layerOk && ImGui::IsMouseClicked(0) && !ImGui::GetIO().WantTextInput)
		{
			stroking = true; strokeErase = eraseMode || io.KeyCtrl; haveLast = false; strokeBefore.clear();
		}
		if (stroking)
		{
			if (ImGui::IsMouseDown(0))
			{
				if (ok && layerOk)
				{
					glm::vec2 cur(hit.x, hit.z);
					if (!haveLast) { stamp(mv->map, P, gnd, cur, R, strokeErase, P.selLayer); lastPos = cur; haveLast = true; }
					else
					{
						float step = std::max(2.0f, R * 0.25f), dist = glm::length(cur - lastPos);
						if (dist >= step)
						{
							int n = (int)(dist / step);
							glm::vec2 dir = (cur - lastPos) / dist;
							for (int i = 1; i <= n; i++) stamp(mv->map, P, gnd, lastPos + dir * (step * i), R, strokeErase, P.selLayer);
							lastPos += dir * (step * n);
						}
					}
				}
			}
			else
			{
				stroking = false;
				std::map<int, Masks> after;
				for (auto& kv : strokeBefore) if (P.cells.count(kv.first)) after[kv.first] = P.cells[kv.first].masks;
				if (!after.empty())
					mv->map->doAction(new PaintAction(strokeBefore, after, strokeErase ? "Advanced texture erase" : "Advanced texture paint"), be);
				strokeBefore.clear();
			}
		}
		if (mv->hovered && ok)
			drawRing(mv, gnd, glm::vec2(hit.x, hit.z), R, (strokeErase && stroking) || eraseMode || io.KeyCtrl ? glm::vec4(1, 0.3f, 0.2f, 1) : glm::vec4(0.2f, 0.9f, 1, 1));
	}

	// ------------------------------------------------------------------ window
	static std::vector<std::string> texFiles;
	static bool texFilesBuilt = false;
	static std::string texFilter;
	static bool openPicker = false;

	static void buildTexFiles()
	{
		texFilesBuilt = true;
		texFiles.clear();
		const std::string prefix = "data\\texture\\";
		for (auto& f : util::FileIO::listAllFiles())
		{
			if (f.size() <= prefix.size()) continue;
			std::string l = f; for (auto& c : l) c = (char)tolower((unsigned char)c);
			if (l.compare(0, prefix.size(), prefix) != 0) continue;
			std::string ext = l.substr(l.find_last_of('.') == std::string::npos ? l.size() : l.find_last_of('.'));
			if (ext != ".bmp" && ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg") continue;
			if (l.find("_at_") != std::string::npos) continue;		// our own output
			texFiles.push_back(f.substr(prefix.size()));
		}
		std::sort(texFiles.begin(), texFiles.end());
	}

	static void thumb(const std::string& file, float size)
	{
		auto t = util::ResourceManager<gl::Texture>::load("data\\texture\\" + file);
		if (t && t->id()) ImGui::Image((ImTextureID)(intptr_t)t->id(), ImVec2(size, size));
		else ImGui::Dummy(ImVec2(size, size));
	}

	static void swapLayers(Project& P, int a, int b)
	{
		std::swap(P.layers[a], P.layers[b]);
		for (auto& kv : P.cells)
		{
			auto& m = kv.second.masks;
			std::vector<uint8_t> va, vb; bool ha = m.count(a), hb = m.count(b);
			if (ha) va = m[a];
			if (hb) vb = m[b];
			m.erase(a); m.erase(b);
			if (hb) m[a] = vb;
			if (ha) m[b] = va;
			if (ha || hb) kv.second.dirty = true;
		}
		P.pending = true;
	}

	static void removeLayer(Project& P, int idx)
	{
		for (auto& kv : P.cells)
		{
			Masks nm; bool had = false;
			for (auto& m : kv.second.masks)
			{
				if (m.first == idx) { had = true; continue; }
				nm[m.first > idx ? m.first - 1 : m.first] = m.second;
			}
			kv.second.masks = nm;
			if (had) kv.second.dirty = true;
		}
		P.layers.erase(P.layers.begin() + idx);
		P.selLayer = std::clamp(P.selLayer, 0, std::max(0, (int)P.layers.size() - 1));
		P.pending = true;
	}

	void showWindow(BrowEdit* be)
	{
		if (!be->activeMapView || !be->activeMapView->map) return;
		Map* map = be->activeMapView->map;
		Project& P = project(map, be);

		// automatic bake once the stroke is done (files are only rewritten when the picture actually changed)
		if (P.autoBake && P.pending && !stroking && !ImGui::IsMouseDown(0))
		{
			std::string msg;
			bake(map, be, P, false, msg);
			statusLine = msg;
		}

		ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Advanced Texture")) { ImGui::End(); return; }

		ImGui::TextWrapped("Left mouse paints the selected layer, Ctrl + left mouse erases. Paint is non-destructive and is baked into %s_at_NNNN.bmp files that the GND uses like any other texture.", mapBase(map).c_str());
		ImGui::Separator();
		ImGui::Text("Layers");
		for (int i = 0; i < (int)P.layers.size(); i++)
		{
			ImGui::PushID(i);
			thumb(P.layers[i].tex, 32);
			ImGui::SameLine();
			if (ImGui::Selectable(P.layers[i].tex.c_str(), P.selLayer == i, 0, ImVec2(0, 32))) P.selLayer = i;
			ImGui::PopID();
		}
		if (ImGui::Button("Add layer...")) { openPicker = true; if (!texFilesBuilt) buildTexFiles(); ImGui::OpenPopup("Pick texture"); }
		bool sel = P.selLayer >= 0 && P.selLayer < (int)P.layers.size();
		if (sel)
		{
			ImGui::SameLine(); if (ImGui::Button("Up") && P.selLayer > 0) { swapLayers(P, P.selLayer, P.selLayer - 1); P.selLayer--; }
			ImGui::SameLine(); if (ImGui::Button("Down") && P.selLayer + 1 < (int)P.layers.size()) { swapLayers(P, P.selLayer, P.selLayer + 1); P.selLayer++; }
			ImGui::SameLine(); if (ImGui::Button("Remove")) { removeLayer(P, P.selLayer); sel = false; }
		}
		if (sel)
		{
			if (ImGui::SliderFloat("Cells per repeat", &P.layers[P.selLayer].tiling, 0.5f, 16.0f)) markLayerDirty(P, P.selLayer);
		}
		if (ImGui::BeginPopup("Pick texture"))
		{
			ImGui::InputText("Filter", &texFilter);
			ImGui::BeginChild("texlist", ImVec2(420, 300), true);
			std::string f = texFilter; for (auto& c : f) c = (char)tolower((unsigned char)c);
			int shown = 0;
			for (auto& t : texFiles)
			{
				if (!f.empty()) { std::string l = t; for (auto& c : l) c = (char)tolower((unsigned char)c); if (l.find(f) == std::string::npos) continue; }
				if (++shown > 300) break;
				if (ImGui::Selectable(t.c_str()))
				{
					P.layers.push_back({ t, 2.0f }); P.selLayer = (int)P.layers.size() - 1;
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndChild();
			if (ImGui::Button("Reload list")) buildTexFiles();
			ImGui::EndPopup();
		}

		ImGui::Separator();
		ImGui::Text("Brush");
		ImGui::SliderFloat("Radius (cells)", &brushRadius, 0.5f, 10.0f);
		float op = brushOpacity * 100, hd = brushHardness * 100;
		if (ImGui::SliderFloat("Opacity %", &op, 1, 100)) brushOpacity = op / 100;
		if (ImGui::SliderFloat("Hardness %", &hd, 0, 95)) brushHardness = hd / 100;
		ImGui::Checkbox("Erase mode", &eraseMode);

		ImGui::Separator();
		ImGui::Text("Baked textures");
		ImGui::InputText("Folder (data\\texture\\...)", &P.folder);
		const char* cp[] = { "Low (14 px per cell)", "Normal (30 px per cell)", "High (62 px per cell)" };
		int ci = P.cellPx == 14 ? 0 : P.cellPx == 62 ? 2 : 1;
		if (ImGui::Combo("Resolution", &ci, cp, 3)) P.cellPx = ci == 0 ? 14 : ci == 2 ? 62 : 30;
		const char* as[] = { "256 x 256", "512 x 512" };
		int ai = P.atlasSize == 256 ? 0 : 1;
		if (ImGui::Combo("Atlas size", &ai, as, 2)) P.atlasSize = ai == 0 ? 256 : 512;
		ImGui::Checkbox("Bake automatically after each stroke", &P.autoBake);
		ImGui::TextDisabled("Changing folder or resolution takes effect with 'Rebuild all'.");

		int dirtyCells = 0, painted = 0;
		for (auto& kv : P.cells) { if (kv.second.dirty) dirtyCells++; if (!isEmptyCell(kv.second)) painted++; }
		int liveAtlases = 0; for (auto& a : P.atlases) if (!a.retired) liveAtlases++;
		ImGui::Text("%d painted cells, %d changed, %d texture file(s)", painted, dirtyCells, liveAtlases);

		if (ImGui::Button("Bake changed textures"))
		{
			std::string msg; bake(map, be, P, false, msg); statusLine = msg;
		}
		ImGui::SameLine();
		if (ImGui::Button("Rebuild all")) ImGui::OpenPopup("Rebuild?");
		if (ImGui::BeginPopup("Rebuild?"))
		{
			ImGui::Text("Re-create every texture with the current folder and resolution?");
			ImGui::Text("The old files are kept until you clean them up.");
			if (ImGui::Button("Rebuild")) { std::string msg; bake(map, be, P, true, msg); statusLine = msg; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		auto unused = unusedAtlases(map, P);
		if (!unused.empty())
		{
			if (ImGui::Button(("Clean up unused files (" + std::to_string(unused.size()) + ")").c_str())) ImGui::OpenPopup("Clean?");
		}
		if (ImGui::BeginPopup("Clean?"))
		{
			ImGui::Text("These generated textures are not used by any tile and will be deleted:");
			for (auto& f : unused) ImGui::BulletText("%s", f.c_str());
			if (ImGui::Button("Delete")) { int n = deleteUnused(map, be, P); statusLine = "Deleted " + std::to_string(n) + " unused file(s)"; ImGui::CloseCurrentPopup(); }
			ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		if (!statusLine.empty()) ImGui::TextWrapped("%s", statusLine.c_str());
		ImGui::End();
	}
}
