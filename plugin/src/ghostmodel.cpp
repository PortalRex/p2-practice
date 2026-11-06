#include "ghostmodel.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "engine.h"
#include "route.h"

namespace ghostmodel {
namespace {

namespace fs = std::filesystem;

struct PlayerModel {
	const char *key;       // in the copies' names
	const char *source;    // in the archive, without extension
};
const PlayerModel kModels[] = {
	{"atlas", "models/player/ballbot/ballbot"},
	{"pbody", "models/player/eggbot/eggbot"},
	{"chell", "models/player/chell/player"},
};
const char kMaterialDir[] = "p2practice\\ghost\\";  // (as the model file lists material folders)

// Lines in the portal colour the ghost is tinted with (rendercolor), lit by nothing. The faces turned away are
// left out as they are for the solid model: drawing their lines too makes the figure twice as dense.
const char kMaterial[] =
	"\"UnlitGeneric\"\n"
	"{\n"
	"\t\"$basetexture\" \"vgui/white\"\n"
	"\t\"$model\" \"1\"\n"
	"\t\"$wireframe\" \"1\"\n"
	"}\n";

// ---- the game's archive (a VPK: a directory file, with the data in numbered files beside it) ----

struct Entry {
	uint32_t crc = 0;
	uint16_t preloadBytes = 0, archive = 0;
	uint32_t offset = 0, length = 0;
	size_t preloadAt = 0;  // in the directory file
};

class Archive {
public:
	bool Open(const fs::path &directoryFile, std::string &error) {
		std::ifstream in(directoryFile, std::ios::binary);
		if (!in) return Fail(error, "can't open " + directoryFile.string());
		directory_.assign(std::istreambuf_iterator<char>(in), {});
		base_ = directoryFile.string();
		base_.erase(base_.size() - strlen("_dir.vpk"));
		if (directory_.size() < 12 || U32(0) != 0x55AA1234) return Fail(error, directoryFile.string() + " isn't a VPK");
		uint32_t version = U32(4), treeSize = U32(8);
		size_t at = version == 2 ? 28 : 12;
		dataStart_ = at + treeSize;
		// extension / folder / name, each list ending with an empty string
		for (std::string extension; ReadString(at, extension) && !extension.empty();) {
			for (std::string folder; ReadString(at, folder) && !folder.empty();) {
				for (std::string name; ReadString(at, name) && !name.empty();) {
					if (at + 18 > directory_.size()) return Fail(error, "the VPK directory is cut short");
					Entry entry;
					memcpy(&entry.crc, &directory_[at], 4);
					memcpy(&entry.preloadBytes, &directory_[at + 4], 2);
					memcpy(&entry.archive, &directory_[at + 6], 2);
					memcpy(&entry.offset, &directory_[at + 8], 4);
					memcpy(&entry.length, &directory_[at + 12], 4);
					at += 18;
					entry.preloadAt = at;
					at += entry.preloadBytes;
					std::string path = (folder == " " ? "" : folder + "/") + name + "." + extension;
					entries_[path] = entry;
				}
			}
		}
		return true;
	}

	bool Read(const std::string &path, std::vector<char> &out, std::string &error) const {
		auto found = entries_.find(path);
		if (found == entries_.end()) return Fail(error, path + " isn't in the archive");
		const Entry &entry = found->second;
		out.assign(directory_.begin() + entry.preloadAt, directory_.begin() + entry.preloadAt + entry.preloadBytes);
		if (entry.length == 0) return true;
		std::vector<char> rest(entry.length);
		if (entry.archive == 0x7FFF) {
			if (dataStart_ + entry.offset + entry.length > directory_.size()) return Fail(error, path + " runs past the archive");
			memcpy(rest.data(), &directory_[dataStart_ + entry.offset], entry.length);
		} else {
			char number[16];
			snprintf(number, sizeof(number), "_%03u.vpk", entry.archive);
			std::ifstream in(base_ + number, std::ios::binary);
			in.seekg(entry.offset);
			if (!in.read(rest.data(), entry.length)) return Fail(error, "can't read " + path + " from " + base_ + number);
		}
		out.insert(out.end(), rest.begin(), rest.end());
		return true;
	}

private:
	static bool Fail(std::string &error, const std::string &why) {
		error = why;
		return false;
	}
	uint32_t U32(size_t at) const {
		uint32_t v;
		memcpy(&v, &directory_[at], 4);
		return v;
	}
	bool ReadString(size_t &at, std::string &out) {
		size_t end = at;
		while (end < directory_.size() && directory_[end]) end++;
		if (end >= directory_.size()) return false;
		out.assign(&directory_[at], end - at);
		at = end + 1;
		return true;
	}

	std::vector<char> directory_;
	std::string base_;
	size_t dataStart_ = 0;
	std::map<std::string, Entry> entries_;
};

// ---- the model file (studiohdr_t, version 49) ----

constexpr size_t kName = 12, kNameSize = 64, kLength = 76, kNumTextures = 204, kTextureIndex = 208, kNumCdTextures = 212, kCdTextureIndex = 216,
                 kHeader2Index = 400, kHeader2Name = 0x14;
constexpr size_t kTextureSize = 64;

int32_t I32(const std::vector<char> &data, size_t at) {
	int32_t v = 0;
	if (at + 4 <= data.size()) memcpy(&v, &data[at], 4);
	return v;
}

std::string CString(const std::vector<char> &data, size_t at) {
	std::string out;
	while (at < data.size() && data[at]) out += data[at++];
	return out;
}

// Points every material folder of the model at the plugin's, and gives the copy its own name inside: the game
// finds a model's vertex and mesh files by that name, and a mod may have replaced the original's (a hat mod does),
// which the copy's files wouldn't match. Collects the material names the model uses.
bool Patch(std::vector<char> &mdl, const std::string &ownName, std::vector<std::string> &materials, std::string &error) {
	if (mdl.size() < 408 || memcmp(mdl.data(), "IDST", 4) != 0 || I32(mdl, 4) != 49) {
		error = "the model file isn't the kind this plugin knows (version 49)";
		return false;
	}
	int32_t textures = I32(mdl, kNumTextures), textureIndex = I32(mdl, kTextureIndex);
	for (int32_t i = 0; i < textures; i++) {
		size_t at = static_cast<size_t>(textureIndex) + static_cast<size_t>(i) * kTextureSize;
		materials.push_back(CString(mdl, at + static_cast<size_t>(I32(mdl, at))));
	}
	int32_t folders = I32(mdl, kNumCdTextures), folderIndex = I32(mdl, kCdTextureIndex);
	int32_t ours = static_cast<int32_t>(mdl.size());
	mdl.insert(mdl.end(), kMaterialDir, kMaterialDir + sizeof(kMaterialDir));  // (with its terminator)
	for (int32_t i = 0; i < folders; i++) memcpy(&mdl[static_cast<size_t>(folderIndex) + static_cast<size_t>(i) * 4], &ours, 4);
	// the name: in the header, and in the second header where there is one (the game prefers that one)
	memset(&mdl[kName], 0, kNameSize);
	memcpy(&mdl[kName], ownName.c_str(), std::min(ownName.size(), kNameSize - 1));
	int32_t header2 = I32(mdl, kHeader2Index);
	if (header2 > 0 && static_cast<size_t>(header2) + kHeader2Name + 4 <= mdl.size()) {
		int32_t nameAt = static_cast<int32_t>(mdl.size()) - header2;
		mdl.insert(mdl.end(), ownName.c_str(), ownName.c_str() + ownName.size() + 1);
		memcpy(&mdl[static_cast<size_t>(header2) + kHeader2Name], &nameAt, 4);
	}
	int32_t length = static_cast<int32_t>(mdl.size());
	memcpy(&mdl[kLength], &length, 4);
	return true;
}

// ---- the mesh file (OptimizedModel::FileHeader_t, version 7) ----

constexpr size_t kVtxNumBodyParts = 28, kVtxBodyPartIndex = 32;

// Makes the mesh file's coarsest level of detail the one drawn at any distance. The game draws the first level
// whose successor's switch distance is beyond the model's (a level at -1 is only for shadows and is left out);
// with every switch distance at zero that is the last level. (The model header's "root LOD" would seem to be
// for this, but set, it makes the engine reject the vertex file.)
bool LowestDetail(std::vector<char> &vtx, std::string &error) {
	if (vtx.size() < 36 || I32(vtx, 0) != 7) {
		error = "the mesh file isn't the kind this plugin knows (version 7)";
		return false;
	}
	int32_t bodyParts = I32(vtx, kVtxNumBodyParts), bodyPartAt = I32(vtx, kVtxBodyPartIndex);
	for (int32_t part = 0; part < bodyParts; part++) {
		size_t partAt = static_cast<size_t>(bodyPartAt) + static_cast<size_t>(part) * 8;
		int32_t models = I32(vtx, partAt), modelAt = I32(vtx, partAt + 4);
		for (int32_t model = 0; model < models; model++) {
			size_t at = partAt + static_cast<size_t>(modelAt) + static_cast<size_t>(model) * 8;
			int32_t lods = I32(vtx, at), lodAt = I32(vtx, at + 4);
			for (int32_t lod = 1; lod < lods; lod++) {
				size_t switchAt = at + static_cast<size_t>(lodAt) + static_cast<size_t>(lod) * 12 + 8;
				if (switchAt + 4 > vtx.size()) {
					error = "the mesh file is cut short";
					return false;
				}
				float distance;
				memcpy(&distance, &vtx[switchAt], 4);
				if (distance < 0) continue;
				distance = 0;
				memcpy(&vtx[switchAt], &distance, 4);
			}
		}
	}
	return true;
}

bool WriteFile(const fs::path &path, const char *data, size_t size, std::string &error) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary);
	if (!out.write(data, static_cast<std::streamsize>(size))) {
		error = "can't write " + path.string();
		return false;
	}
	return true;
}

fs::path GameFolder() {
	return fs::u8path(engine::GameDirectory());
}

// The archives a player model may come from, in the order to look: the game folder's own (a mod's own robots),
// then the main game's (the robots as the run had them), then the add-on folders beside it (the official ones
// don't replace the player models; a hat mod placed there does, and the ghosts shouldn't wear its hats). The
// game folder may be a mod's, beside portal2 or further down the tree.
std::vector<fs::path> ArchiveFiles() {
	std::vector<fs::path> files;
	std::error_code ec;
	auto add = [&](const fs::path &folder) {
		if (fs::is_regular_file(folder / "pak01_dir.vpk", ec)) files.push_back(folder / "pak01_dir.vpk");
	};
	add(GameFolder());
	fs::path folder = GameFolder();
	for (int up = 0; up < 4 && folder.has_parent_path(); up++) {
		folder = folder.parent_path();
		if (!fs::is_regular_file(folder / "portal2" / "pak01_dir.vpk", ec)) continue;  // (not the main game's folder)
		add(folder / "portal2");
		for (int addon = 1; addon <= 9; addon++) add(folder / ("portal2_dlc" + std::to_string(addon)));
		break;
	}
	return files;
}

std::string CopyName(const PlayerModel &model, bool lowDetail) {
	return std::string(lowDetail ? "ghostlow_" : "ghost_") + model.key;
}

// Reads a model's three files from the first archive that holds the model (all three from the same one: they
// belong together)
bool ReadModel(const PlayerModel &model, std::vector<char> &mdl, std::vector<char> &vvd, std::vector<char> &vtx, std::string &vtxExtension,
               std::string &error) {
	std::vector<fs::path> files = ArchiveFiles();
	if (files.empty()) {
		error = "no pak01_dir.vpk near " + GameFolder().string();
		return false;
	}
	for (const fs::path &file : files) {
		Archive archive;
		if (!archive.Open(file, error)) return false;
		std::string ignored;
		if (!archive.Read(std::string(model.source) + ".mdl", mdl, ignored)) continue;
		if (!archive.Read(std::string(model.source) + ".vvd", vvd, error)) return false;
		// (the mesh file: the one for this renderer, or the one for any)
		for (const char *extension : {".dx90.vtx", ".vtx"}) {
			vtxExtension = extension;
			if (archive.Read(std::string(model.source) + extension, vtx, ignored)) return true;
		}
		error = std::string(model.source) + " has no mesh file in " + file.string();
		return false;
	}
	error = std::string(model.source) + ".mdl isn't in any of the game's archives";
	return false;
}

// Makes one model's copy (and the materials), unless it is already there
bool Make(const PlayerModel &model, bool lowDetail, std::string &error) {
	fs::path target = GameFolder() / "models" / "p2practice" / CopyName(model, lowDetail);
	std::error_code ec;
	if (fs::is_regular_file(target.string() + ".mdl", ec) && fs::is_regular_file(target.string() + ".vvd", ec) &&
	    fs::is_regular_file(target.string() + ".dx90.vtx", ec))
		return true;
	std::vector<char> mdl, vvd, vtx;
	std::string vtxExtension;
	if (!ReadModel(model, mdl, vvd, vtx, vtxExtension, error)) return false;
	std::vector<std::string> materials;
	if (!Patch(mdl, "p2practice/" + CopyName(model, lowDetail) + ".mdl", materials, error)) return false;
	if (lowDetail && !LowestDetail(vtx, error)) return false;
	// (the mesh file is written under the name this renderer looks for, whatever the source called it)
	if (!WriteFile(target.string() + ".mdl", mdl.data(), mdl.size(), error) || !WriteFile(target.string() + ".vvd", vvd.data(), vvd.size(), error) ||
	    !WriteFile(target.string() + ".dx90.vtx", vtx.data(), vtx.size(), error))
		return false;
	for (const std::string &material : materials) {
		fs::path file = GameFolder() / "materials" / "p2practice" / "ghost" / fs::u8path(material + ".vmt");
		if (!fs::is_regular_file(file, ec) && !WriteFile(file, kMaterial, strlen(kMaterial), error)) return false;
	}
	return true;
}

bool g_complained = false;

}  // namespace

std::string Wireframe(int team, bool lowDetail) {
	const PlayerModel &model = team == route::kTeamBlue ? kModels[0] : team == route::kTeamOrange ? kModels[1] : kModels[2];
	std::string error;
	bool ok = Make(model, lowDetail, error);
	if (!ok) {
		if (!g_complained) engine::Warn("Wireframe ghosts aren't available: %s. Ghosts are shown as solid models.\n", error.c_str());
		g_complained = true;
		return "";
	}
	return "models/p2practice/" + CopyName(model, lowDetail) + ".mdl";
}

}  // namespace ghostmodel
