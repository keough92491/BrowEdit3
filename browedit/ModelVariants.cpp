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

		bool writeTexture(BrowEdit* be, const std::string& oldName, const std::string& newName, std::string& err)
		{
			auto im = advtex::getImg(oldName);
			if (!im) { err = "Cannot read texture " + oldName; return false; }
			std::vector<unsigned char> px(im->px.size());
			bool alpha = false;
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
		if (!visible) return;
		ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Model variants", &visible)) { ImGui::End(); return; }
		auto mv = be->activeMapView;
		if (!mv || !mv->map) { ImGui::TextUnformatted("Open a map first."); ImGui::End(); return; }
		Map* map = mv->map;

		// source: the first selected model, or typed in
		if (ImGui::Button("Use selected model"))
			for (auto n : map->selectedNodes)
				if (auto m = n->getComponent<RswModel>()) { srcModel = "data\\model\\" + util::utf8_to_iso_8859_1(m->fileName); thumbsDirty = true; enabled.clear(); break; }
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
		if (changed) thumbsDirty = true;
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
				if (enabled[t])
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
