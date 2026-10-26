#include "ModelVariants.h"
#include <browedit/BrowEdit.h>
#include <browedit/Map.h>
#include <browedit/Node.h>
#include <browedit/MapView.h>
#include <browedit/components/Rsw.h>
#include <browedit/components/Rsm.h>
#include <browedit/actions/GroupAction.h>
#include <browedit/actions/ModelChangeAction.h>
#include <browedit/advtex/AdvTex.h>
#include <browedit/util/Util.h>
#include <browedit/util/ResourceManager.h>
#include <browedit/gl/Texture.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glad/gl.h>
#include <stb/stb_image_write.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

namespace modelvariants
{
	bool visible = false;

	namespace
	{
		struct Params
		{
			float hue = 0.0f, sat = 1.0f, bright = 1.0f, contrast = 1.0f, tintAmount = 0.0f;
			float tint[3] = { 0.6f, 0.8f, 1.0f };
			bool operator==(const Params& o) const { return hue == o.hue && sat == o.sat && bright == o.bright && contrast == o.contrast && tintAmount == o.tintAmount && tint[0] == o.tint[0] && tint[1] == o.tint[1] && tint[2] == o.tint[2]; }
		};
		Params params, thumbParams;
		bool thumbsDirty = true;
		std::string srcModel;				// "data\model\..." in ISO
		std::string suffix = "_t";
		std::string texFolder = "variants\\";	// below data\texture
		std::string modelFolder;				// below data\model, empty = next to the original
		int swapMode = 1;						// 0 nothing, 1 selected, 2 all that use the model
		std::map<std::string, bool> enabled;
		std::string result;
		struct Thumb { gl::Texture* before = nullptr; gl::Texture* after = nullptr; };
		std::map<std::string, Thumb> thumbs;

		bool isKey(const unsigned char* p) { return p[0] > 240 && p[1] < 16 && p[2] > 240; }		// the client's transparent magenta

		void rgb2hsv(float r, float g, float b, float& h, float& s, float& v)
		{
			float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
			v = mx; s = mx > 0 ? d / mx : 0; h = 0;
			if (d > 0)
			{
				if (mx == r) h = std::fmod((g - b) / d, 6.0f);
				else if (mx == g) h = (b - r) / d + 2.0f;
				else h = (r - g) / d + 4.0f;
				h *= 60.0f; if (h < 0) h += 360.0f;
			}
		}
		void hsv2rgb(float h, float s, float v, float& r, float& g, float& b)
		{
			float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
			float rr = 0, gg = 0, bb = 0;
			int i = (int)(h / 60.0f) % 6;
			switch (i) { case 0: rr = c; gg = x; break; case 1: rr = x; gg = c; break; case 2: gg = c; bb = x; break; case 3: gg = x; bb = c; break; case 4: rr = x; bb = c; break; default: rr = c; bb = x; break; }
			r = rr + m; g = gg + m; b = bb + m;
		}
		void transform(const Params& p, const unsigned char* in, unsigned char* out)
		{
			float r = in[0] / 255.0f, g = in[1] / 255.0f, b = in[2] / 255.0f, h, s, v;
			rgb2hsv(r, g, b, h, s, v);
			h = std::fmod(h + p.hue + 720.0f, 360.0f);
			s = std::clamp(s * p.sat, 0.0f, 1.0f);
			v = std::clamp(v * p.bright, 0.0f, 1.0f);
			hsv2rgb(h, s, v, r, g, b);
			float c[3] = { r, g, b };
			for (int i = 0; i < 3; i++)
			{
				c[i] = (c[i] - 0.5f) * p.contrast + 0.5f;
				// the tint keeps the light/dark structure of the texture
				float lum = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
				c[i] = c[i] * (1 - p.tintAmount) + (p.tint[i] * lum * 1.6f) * p.tintAmount;
				out[i] = (unsigned char)std::clamp((int)std::lround(c[i] * 255.0f), 0, 255);
			}
		}

		std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
		std::string stripName(const std::string& n, std::string& ext)
		{
			auto sl = n.find_last_of("\\/");
			std::string b = sl == std::string::npos ? n : n.substr(sl + 1);
			auto dot = b.rfind('.');
			ext = dot == std::string::npos ? "" : b.substr(dot);
			return dot == std::string::npos ? b : b.substr(0, dot);
		}
		std::string newTextureName(const std::string& oldName)
		{
			std::string ext; auto base = stripName(oldName, ext);
			std::string folder = texFolder;
			std::replace(folder.begin(), folder.end(), '/', '\\');
			if (!folder.empty() && folder.back() != '\\') folder += "\\";
			if (lower(ext) != ".bmp" && lower(ext) != ".tga" && lower(ext) != ".png" && lower(ext) != ".jpg") ext = ".bmp";
			return folder + base + suffix + ext;
		}

		void updateThumb(gl::Texture*& tex, const advtex::Img& im, const Params* p)
		{
			const int T = 64;
			std::vector<unsigned char> px((size_t)T * T * 4);
			for (int y = 0; y < T; y++)
				for (int x = 0; x < T; x++)
				{
					int sx = std::min(im.w - 1, x * im.w / T), sy = std::min(im.h - 1, y * im.h / T);
					const unsigned char* s = &im.px[((size_t)sy * im.w + sx) * 4];
					unsigned char* d = &px[((size_t)y * T + x) * 4];
					if (p && !isKey(s)) transform(*p, s, d); else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
					d[3] = 255;
				}
			if (!tex) tex = new gl::Texture(T, T);
			glBindTexture(GL_TEXTURE_2D, tex->id());
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, T, T, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		}

		// ---------------------------------------------------------------- brush painting (on the texture, with the model's UV faces shown)
		struct PaintTex
		{
			int w = 0, h = 0;
			std::vector<unsigned char> orig, overlay, comp;		// RGBA; overlay alpha = paint coverage
			std::string name;
			std::vector<unsigned char> uvMask;						// 255 where a face of this model uses the pixel
			std::vector<float> lines;								// uv edges (u0,v0,u1,v1) for the wireframe
			gl::Texture* view = nullptr;
			std::vector<std::vector<unsigned char>> undo;
			bool dirtyView = true, dirtyModel = false;
			bool hasPaint = false;
		};
		std::map<std::string, PaintTex> paintTex;
		std::string paintCur;
		float brushSize = 24.0f, brushHard = 0.6f, brushOpacity = 1.0f, zoom = 1.0f;
		float brushColor[3] = { 0.8f, 0.2f, 0.1f };
		int brushMode = 0;				// 0 colour, 1 stamp texture
		bool onlyUv = true, showUv = true, livePreview = true, eraseMode = false;
		std::string stampTex;
		float stampTiling = 1.0f;
		ImVec2 pan(0, 0);
		bool painting = false;
		ImVec2 lastPaintPos(-1, -1);
		bool previewActive = false;
		std::set<std::string> previewTouched;

		bool needsBase(const PaintTex& pt, int i) { return !isKey(&pt.orig[i * 4]); }

		void compositeRange(PaintTex& pt, int x0, int y0, int x1, int y1, bool = true)
		{
			bool useParams = enabled.count(pt.name) ? enabled[pt.name] : true;
			x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(pt.w - 1, x1); y1 = std::min(pt.h - 1, y1);
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
				{
					size_t i = (size_t)y * pt.w + x;
					unsigned char* d = &pt.comp[i * 4];
					const unsigned char* o = &pt.orig[i * 4];
					if (isKey(o)) { d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; d[3] = o[3]; continue; }
					unsigned char base[3];
					if (useParams) transform(params, o, base); else { base[0] = o[0]; base[1] = o[1]; base[2] = o[2]; }
					const unsigned char* v = &pt.overlay[i * 4];
					float a = v[3] / 255.0f;
					for (int c = 0; c < 3; c++) d[c] = (unsigned char)std::lround(base[c] * (1 - a) + v[c] * a);
					d[3] = o[3];
				}
		}

		void rasterTri(PaintTex& pt, glm::vec2 a, glm::vec2 b, glm::vec2 c)
		{
			a *= glm::vec2((float)pt.w, (float)pt.h); b *= glm::vec2((float)pt.w, (float)pt.h); c *= glm::vec2((float)pt.w, (float)pt.h);
			int x0 = (int)std::floor(std::min({ a.x, b.x, c.x })) - 1, x1 = (int)std::ceil(std::max({ a.x, b.x, c.x })) + 1;
			int y0 = (int)std::floor(std::min({ a.y, b.y, c.y })) - 1, y1 = (int)std::ceil(std::max({ a.y, b.y, c.y })) + 1;
			x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(pt.w - 1, x1); y1 = std::min(pt.h - 1, y1);
			float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
			if (std::fabs(den) < 1e-6f) return;
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
				{
					float px = x + 0.5f, py = y + 0.5f;
					float l1 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / den;
					float l2 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / den;
					float l3 = 1 - l1 - l2;
					const float e = -0.06f;		// a hair outside, so seams are covered
					if (l1 >= e && l2 >= e && l3 >= e) pt.uvMask[(size_t)y * pt.w + x] = 255;
				}
		}

		PaintTex* getPaint(Rsm* rsm, const std::string& texName, int texIndex)
		{
			auto it = paintTex.find(texName);
			if (it != paintTex.end()) return &it->second;
			auto im = advtex::getImg(texName);
			if (!im) return nullptr;
			PaintTex pt; pt.name = texName; pt.w = im->w; pt.h = im->h;
			pt.orig = im->px;
			pt.overlay.assign(pt.orig.size(), 0);
			pt.comp = pt.orig;
			pt.uvMask.assign((size_t)pt.w * pt.h, 0);
			rsm->rootMesh->foreach([&](Rsm::Mesh* m)
				{
					for (auto& f : m->faces)
					{
						if (f.texId < 0 || f.texId >= (int)m->textures.size() || m->textures[f.texId] != texIndex) continue;
						glm::vec2 uv[3];
						bool ok = true;
						for (int k = 0; k < 3; k++) { if (f.texCoordIds[k] >= m->texCoords.size()) { ok = false; break; } uv[k] = m->texCoords[f.texCoordIds[k]]; }
						if (!ok) continue;
						rasterTri(pt, uv[0], uv[1], uv[2]);
						for (int k = 0; k < 3; k++) { auto& q = uv[(k + 1) % 3]; pt.lines.insert(pt.lines.end(), { uv[k].x, uv[k].y, q.x, q.y }); }
					}
				});
			return &(paintTex[texName] = std::move(pt));
		}

		void uploadView(PaintTex& pt)
		{
			if (!pt.view) pt.view = new gl::Texture(pt.w, pt.h);
			glBindTexture(GL_TEXTURE_2D, pt.view->id());
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pt.w, pt.h, GL_RGBA, GL_UNSIGNED_BYTE, pt.comp.data());
			pt.dirtyView = false;
		}

		void uploadToModelTexture(const std::string& name, const std::vector<unsigned char>& px, int w, int h)
		{
			auto t = util::ResourceManager<gl::Texture>::load("data\\texture\\" + name);
			if (!t || !t->id() || t->width != w || t->height != h) return;
			glBindTexture(GL_TEXTURE_2D, t->id());
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			glGenerateMipmap(GL_TEXTURE_2D);
			previewTouched.insert(name);
		}

		// puts the real pictures back on the model in the map
		void restorePreview()
		{
			for (auto& kv : paintTex)
				if (previewTouched.count(kv.first)) uploadToModelTexture(kv.first, kv.second.orig, kv.second.w, kv.second.h);
			previewTouched.clear();
			previewActive = false;
		}

		void resetPaint()
		{
			restorePreview();
			for (auto& kv : paintTex) delete kv.second.view;
			paintTex.clear();
			paintCur.clear();
		}

		float stampSample(const advtex::Img& im, float u, float v, int c)
		{
			int x = ((int)std::floor(u * im.w) % im.w + im.w) % im.w, y = ((int)std::floor(v * im.h) % im.h + im.h) % im.h;
			return im.px[((size_t)y * im.w + x) * 4 + c];
		}

		void stamp(PaintTex& pt, float cx, float cy, bool erase)
		{
			const float r = std::max(1.0f, brushSize * 0.5f);
			int x0 = (int)std::floor(cx - r), x1 = (int)std::ceil(cx + r), y0 = (int)std::floor(cy - r), y1 = (int)std::ceil(cy + r);
			const advtex::Img* st = (brushMode == 1 && !stampTex.empty()) ? advtex::getImg(stampTex) : nullptr;
			for (int y = std::max(0, y0); y <= std::min(pt.h - 1, y1); y++)
				for (int x = std::max(0, x0); x <= std::min(pt.w - 1, x1); x++)
				{
					float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) / r;
					if (d >= 1.0f) continue;
					size_t i = (size_t)y * pt.w + x;
					if (onlyUv && !pt.uvMask[i]) continue;
					if (isKey(&pt.orig[i * 4])) continue;
					float a = (d <= brushHard ? 1.0f : 1.0f - (d - brushHard) / (1.0f - brushHard + 1e-4f)) * brushOpacity;
					a = std::clamp(a, 0.0f, 1.0f);
					unsigned char* v = &pt.overlay[i * 4];
					if (erase) { v[3] = (unsigned char)std::lround(v[3] * (1 - a)); continue; }
					float col[3];
					if (st) for (int c = 0; c < 3; c++) col[c] = stampSample(*st, (x + 0.5f) / pt.w * stampTiling * 4.0f, (y + 0.5f) / pt.h * stampTiling * 4.0f, c);
					else for (int c = 0; c < 3; c++) col[c] = brushColor[c] * 255.0f;
					float oa = v[3] / 255.0f, na = a + oa * (1 - a);
					if (na <= 0) continue;
					for (int c = 0; c < 3; c++) v[c] = (unsigned char)std::clamp((int)std::lround((col[c] * a + v[c] * oa * (1 - a)) / na), 0, 255);
					v[3] = (unsigned char)std::lround(na * 255.0f);
				}
			pt.hasPaint = true;
			compositeRange(pt, x0, y0, x1, y1, true);
			pt.dirtyView = true; pt.dirtyModel = true;
		}

		void drawPaintUi(Rsm* rsm)
		{
			ImGui::TextWrapped("Paint straight on a texture of the model. The UV lines show where the model's faces sit, so you can see what you are painting. With 'Preview on the model' on, the model in the map shows the paint live; nothing is saved until you press Generate variant.");
			if (ImGui::BeginCombo("Texture", paintCur.empty() ? "(choose)" : util::iso_8859_1_to_utf8(paintCur).c_str()))
			{
				for (auto& t : rsm->textures)
					if (ImGui::Selectable(util::iso_8859_1_to_utf8(t).c_str(), t == paintCur)) { paintCur = t; pan = ImVec2(0, 0); }
				ImGui::EndCombo();
			}
			int texIndex = -1;
			for (size_t i = 0; i < rsm->textures.size(); i++) if (rsm->textures[i] == paintCur) texIndex = (int)i;
			if (texIndex < 0) return;
			auto pt = getPaint(rsm, paintCur, texIndex);
			if (!pt) { ImGui::TextUnformatted("Cannot read this texture."); return; }

			ImGui::RadioButton("Colour", &brushMode, 0); ImGui::SameLine(); ImGui::RadioButton("Texture stamp", &brushMode, 1);
			if (brushMode == 0) { ImGui::SameLine(); ImGui::ColorEdit3("##bc", brushColor, ImGuiColorEditFlags_NoInputs); }
			else { ImGui::InputText("Stamp texture (below data\\texture)", &stampTex); ImGui::SliderFloat("Stamp scale", &stampTiling, 0.25f, 8.0f, "%.2f"); }
			ImGui::SliderFloat("Size (px)", &brushSize, 2.0f, 200.0f, "%.0f");
			ImGui::SliderFloat("Hardness", &brushHard, 0.0f, 0.95f, "%.2f");
			ImGui::SliderFloat("Opacity", &brushOpacity, 0.05f, 1.0f, "%.2f");
			ImGui::Checkbox("Only on model faces", &onlyUv); ImGui::SameLine();
			ImGui::Checkbox("Show UV lines", &showUv); ImGui::SameLine();
			if (ImGui::Checkbox("Preview on the model", &livePreview) && !livePreview) restorePreview();
			if (ImGui::Button("Undo") && !pt->undo.empty()) { pt->overlay = pt->undo.back(); pt->undo.pop_back(); compositeRange(*pt, 0, 0, pt->w, pt->h, true); pt->dirtyView = pt->dirtyModel = true; }
			ImGui::SameLine();
			if (ImGui::Button("Clear paint")) { pt->undo.push_back(pt->overlay); std::fill(pt->overlay.begin(), pt->overlay.end(), 0); pt->hasPaint = false; compositeRange(*pt, 0, 0, pt->w, pt->h, true); pt->dirtyView = pt->dirtyModel = true; }
			ImGui::SameLine(); ImGui::TextUnformatted("Left paints, Ctrl or right erases, middle drag moves, wheel zooms");

			// canvas
			ImVec2 avail = ImGui::GetContentRegionAvail();
			float side = std::max(200.0f, std::min(avail.x, 520.0f));
			ImVec2 origin = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##canvas", ImVec2(side, side), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			bool hovered = ImGui::IsItemHovered();
			auto& io = ImGui::GetIO();
			float fit = side / (float)std::max(pt->w, pt->h);
			if (hovered && io.MouseWheel != 0) zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 0.3f, 12.0f);
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { pan.x += io.MouseDelta.x; pan.y += io.MouseDelta.y; }
			float sc = fit * zoom;
			ImVec2 img0(origin.x + pan.x, origin.y + pan.y), img1(img0.x + pt->w * sc, img0.y + pt->h * sc);
			if (pt->dirtyView || !pt->view) uploadView(*pt);
			auto dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(origin, ImVec2(origin.x + side, origin.y + side), true);
			dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(40, 40, 40, 255));
			dl->AddImage((ImTextureID)(intptr_t)pt->view->id(), img0, img1);
			if (showUv)
				for (size_t i = 0; i + 3 < pt->lines.size(); i += 4)
					dl->AddLine(ImVec2(img0.x + pt->lines[i] * pt->w * sc, img0.y + pt->lines[i + 1] * pt->h * sc), ImVec2(img0.x + pt->lines[i + 2] * pt->w * sc, img0.y + pt->lines[i + 3] * pt->h * sc), IM_COL32(255, 255, 0, 110));
			if (hovered)
				dl->AddCircle(io.MousePos, std::max(2.0f, brushSize * 0.5f * sc), IM_COL32(255, 255, 255, 220), 32);
			dl->PopClipRect();

			bool lDown = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left), rDown = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right);
			if (lDown || rDown)
			{
				bool erase = rDown || io.KeyCtrl;
				float px = (io.MousePos.x - img0.x) / sc, py = (io.MousePos.y - img0.y) / sc;
				if (!painting) { painting = true; pt->undo.push_back(pt->overlay); if (pt->undo.size() > 15) pt->undo.erase(pt->undo.begin()); lastPaintPos = ImVec2(px, py); stamp(*pt, px, py, erase); }
				else
				{
					float dx = px - lastPaintPos.x, dy = py - lastPaintPos.y, dist = std::sqrt(dx * dx + dy * dy), step = std::max(1.0f, brushSize * 0.2f);
					int n = (int)(dist / step);
					for (int i = 1; i <= n; i++) stamp(*pt, lastPaintPos.x + dx / dist * step * i, lastPaintPos.y + dy / dist * step * i, erase);
					if (n > 0) lastPaintPos = ImVec2(lastPaintPos.x + dx / dist * step * n, lastPaintPos.y + dy / dist * step * n);
				}
			}
			else painting = false;
			if (pt->dirtyModel && livePreview) { uploadToModelTexture(paintCur, pt->comp, pt->w, pt->h); previewActive = true; pt->dirtyModel = false; }
		}

		bool writeTexture(BrowEdit* be, const std::string& oldName, const std::string& newName, std::string& err)
		{
			auto im = advtex::getImg(oldName);
			if (!im) { err = "Cannot read texture " + oldName; return false; }
			std::vector<unsigned char> px(im->px.size());
			bool alpha = false;
			auto pit = paintTex.find(oldName);
			if (pit != paintTex.end() && pit->second.comp.size() == px.size())
			{
				compositeRange(pit->second, 0, 0, pit->second.w, pit->second.h);
				px = pit->second.comp;
				for (size_t i = 3; i < px.size(); i += 4) if (px[i] < 255) { alpha = true; break; }
			}
			else
			for (size_t i = 0; i < px.size(); i += 4)
			{
				const unsigned char* s = &im->px[i];
				if (isKey(s)) { px[i] = s[0]; px[i + 1] = s[1]; px[i + 2] = s[2]; }
				else transform(params, s, &px[i]);
				px[i + 3] = s[3];
				if (s[3] < 255) alpha = true;
			}
			std::string path = be->config.ropath + "data\\texture\\" + newName;
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			std::string ext = lower(newName.substr(newName.size() - 4));
			int ok = 0;
			if (ext == ".tga") ok = stbi_write_tga(path.c_str(), im->w, im->h, 4, px.data());
			else if (ext == ".png") ok = stbi_write_png(path.c_str(), im->w, im->h, 4, px.data(), im->w * 4);
			else if (ext == ".jpg") { std::vector<unsigned char> rgb((size_t)im->w * im->h * 3); for (size_t i = 0, j = 0; i < px.size(); i += 4, j += 3) { rgb[j] = px[i]; rgb[j + 1] = px[i + 1]; rgb[j + 2] = px[i + 2]; } ok = stbi_write_jpg(path.c_str(), im->w, im->h, 3, rgb.data(), 95); }
			else
			{
				std::vector<unsigned char> rgb((size_t)im->w * im->h * 3);
				for (size_t i = 0, j = 0; i < px.size(); i += 4, j += 3) { rgb[j] = px[i]; rgb[j + 1] = px[i + 1]; rgb[j + 2] = px[i + 2]; }
				ok = stbi_write_bmp(path.c_str(), im->w, im->h, 3, rgb.data());
				(void)alpha;
			}
			if (!ok) { err = "Cannot write " + path; return false; }
			return true;
		}
	}

	void showWindow(BrowEdit* be)
	{
		if (!visible) { if (previewActive) restorePreview(); return; }
		ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Model variants", &visible)) { ImGui::End(); return; }
		auto mv = be->activeMapView;
		if (!mv || !mv->map) { ImGui::TextUnformatted("Open a map first."); ImGui::End(); return; }
		Map* map = mv->map;

		// source: the first selected model, or typed in
		if (ImGui::Button("Use selected model"))
			for (auto n : map->selectedNodes)
				if (auto m = n->getComponent<RswModel>()) { resetPaint(); srcModel = "data\\model\\" + util::utf8_to_iso_8859_1(m->fileName); thumbsDirty = true; enabled.clear(); break; }
		ImGui::SameLine();
		ImGui::TextWrapped("%s", srcModel.empty() ? "Select a model in the map, then press the button" : util::iso_8859_1_to_utf8(srcModel).c_str());
		if (srcModel.empty()) { ImGui::End(); return; }

		auto rsm = util::ResourceManager<Rsm>::load(srcModel);
		if (!rsm || !rsm->loaded) { ImGui::TextUnformatted("Could not load that model."); ImGui::End(); return; }
		ImGui::Text("Model version %d.%d, %d textures", rsm->version >> 8, rsm->version & 0xFF, (int)rsm->textures.size());
		ImGui::Separator();

		bool changed = false;
		changed |= ImGui::SliderFloat("Hue shift", &params.hue, -180.0f, 180.0f, "%.0f deg");
		changed |= ImGui::SliderFloat("Saturation", &params.sat, 0.0f, 2.0f, "%.2f");
		changed |= ImGui::SliderFloat("Brightness", &params.bright, 0.2f, 2.0f, "%.2f");
		changed |= ImGui::SliderFloat("Contrast", &params.contrast, 0.5f, 1.8f, "%.2f");
		changed |= ImGui::SliderFloat("Tint amount", &params.tintAmount, 0.0f, 1.0f, "%.2f");
		changed |= ImGui::ColorEdit3("Tint colour", params.tint, ImGuiColorEditFlags_NoInputs);
		ImGui::TextUnformatted("Presets:");
		struct Preset { const char* n; Params p; };
		static const Preset presets[] = {
			{ "Reset", Params() },
			{ "Snow", { 0, 0.35f, 1.35f, 1.0f, 0.55f, { 0.85f, 0.95f, 1.1f } } },
			{ "Autumn", { -35, 1.1f, 1.0f, 1.05f, 0.25f, { 1.0f, 0.65f, 0.3f } } },
			{ "Night", { 10, 0.8f, 0.55f, 1.05f, 0.45f, { 0.35f, 0.45f, 0.9f } } },
			{ "Rusty", { -15, 0.9f, 0.9f, 1.1f, 0.4f, { 0.75f, 0.4f, 0.2f } } },
			{ "Ice", { 150, 0.7f, 1.15f, 1.0f, 0.4f, { 0.6f, 0.9f, 1.1f } } },
			{ "Dead", { 0, 0.3f, 0.75f, 1.1f, 0.3f, { 0.6f, 0.5f, 0.35f } } },
		};
		for (auto& pr : presets) { ImGui::SameLine(); if (ImGui::SmallButton(pr.n)) { params = pr.p; changed = true; } }
		if (changed)
		{
			thumbsDirty = true;
			if (livePreview)
				for (size_t i = 0; i < rsm->textures.size(); i++)
					if (auto pt = getPaint(rsm, rsm->textures[i], (int)i)) { compositeRange(*pt, 0, 0, pt->w, pt->h); pt->dirtyView = pt->dirtyModel = true; }
		}
		ImGui::NewLine();
		ImGui::Separator();

		if (thumbsDirty && !(thumbParams == params && false)) { thumbParams = params; }
		ImGui::Text("Textures (tick the ones to recolour)");
		for (size_t i = 0; i < rsm->textures.size(); i++)
		{
			const std::string& t = rsm->textures[i];
			if (!enabled.count(t)) enabled[t] = true;
			ImGui::PushID((int)i);
			ImGui::Checkbox("##on", &enabled[t]);
			ImGui::SameLine();
			auto im = advtex::getImg(t);
			auto& th = thumbs[t];
			if (im && (thumbsDirty || !th.after))
			{
				if (!th.before) updateThumb(th.before, *im, nullptr);
				updateThumb(th.after, *im, enabled[t] ? &params : nullptr);
			}
			if (th.before) { ImGui::Image((ImTextureID)(intptr_t)th.before->id(), ImVec2(48, 48)); ImGui::SameLine(); }
			if (th.after) { ImGui::Image((ImTextureID)(intptr_t)th.after->id(), ImVec2(48, 48)); ImGui::SameLine(); }
			ImGui::TextWrapped("%s -> %s", util::iso_8859_1_to_utf8(t).c_str(), util::iso_8859_1_to_utf8(newTextureName(t)).c_str());
			ImGui::PopID();
		}
		thumbsDirty = false;
		for (auto& kv : paintTex)
			if (kv.second.dirtyModel && livePreview) { uploadToModelTexture(kv.first, kv.second.comp, kv.second.w, kv.second.h); previewActive = true; kv.second.dirtyModel = false; }
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Paint on the textures (brush)"))
			drawPaintUi(rsm);
		ImGui::Separator();

		ImGui::InputText("Name suffix", &suffix);
		ImGui::InputText("Texture folder (below data\\texture)", &texFolder);
		ImGui::InputText("Model folder (below data\\model, empty = same as the original)", &modelFolder);
		ImGui::TextUnformatted("In the map after generating:");
		ImGui::RadioButton("Leave the instances", &swapMode, 0); ImGui::SameLine();
		ImGui::RadioButton("Swap the selected ones", &swapMode, 1); ImGui::SameLine();
		ImGui::RadioButton("Swap all using this model", &swapMode, 2);

		if (ImGui::Button("Generate variant"))
		{
			result.clear();
			std::string err;
			std::map<std::string, std::string> names;
			bool ok = true;
			for (auto& t : rsm->textures)
				if (enabled[t] || (paintTex.count(t) && paintTex[t].hasPaint))
				{
					std::string nn = newTextureName(t);
					if (rsm->version < 0x0202 && nn.size() > 39) { result = "Texture name too long for model version 1.x (39 characters): " + nn + "\nUse a shorter texture folder or suffix."; ok = false; break; }
					if (!writeTexture(be, t, nn, err)) { result = err; ok = false; break; }
					names[t] = nn;
				}
			std::string newModel;
			if (ok)
			{
				std::string rel = srcModel.substr(11);		// below data\model
				std::string dir = ""; auto sl = rel.find_last_of("\\/");
				if (sl != std::string::npos) dir = rel.substr(0, sl + 1);
				std::string ext; auto base = stripName(rel, ext);
				std::string folder = modelFolder;
				std::replace(folder.begin(), folder.end(), '/', '\\');
				if (!folder.empty() && folder.back() != '\\') folder += "\\";
				newModel = "data\\model\\" + (modelFolder.empty() ? dir : folder) + base + suffix + ext;
				std::string path = be->config.ropath + newModel;
				std::error_code ec;
				std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
				ok = rsm->saveWithTextures(path, names, err);
				if (!ok) result = err;
			}
			if (ok)
			{
				restorePreview();
				result = "Saved " + util::iso_8859_1_to_utf8(newModel) + " and " + std::to_string(names.size()) + " texture(s).";
				if (swapMode > 0)
				{
					auto ga = new GroupAction();
					int count = 0;
					std::vector<Node*> targets;
					if (swapMode == 1) targets = map->selectedNodes;
					else map->rootNode->traverse([&](Node* n) { targets.push_back(n); });
					for (auto n : targets)
					{
						auto m = n->getComponent<RswModel>();
						if (!m || "data\\model\\" + util::utf8_to_iso_8859_1(m->fileName) != srcModel) continue;
						ga->addAction(new ModelChangeAction(n, newModel));
						count++;
					}
					if (count > 0) { map->doAction(ga, be); result += " Swapped " + std::to_string(count) + " instance(s)."; }
					else { delete ga; result += " No matching instances to swap."; }
				}
			}
		}
		if (!result.empty()) ImGui::TextWrapped("%s", result.c_str());
		ImGui::End();
	}
}
