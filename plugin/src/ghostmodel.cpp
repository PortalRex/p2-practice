#include "ghostmodel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
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

// A path as text for a message (UTF-8, whatever the folder is called)
std::string Shown(const fs::path &path) { return path.u8string(); }

// Every size and offset in the directory comes from the file: each is checked against what is really there
// before it is used.
class Archive {
public:
	bool Open(const fs::path &directoryFile, std::string &error) {
		constexpr wchar_t kDirectorySuffix[] = L"_dir.vpk";
		std::ifstream in(directoryFile, std::ios::binary);
		if (!in) return Fail(error, "can't open " + Shown(directoryFile));
		directory_.assign(std::istreambuf_iterator<char>(in), {});
		base_ = directoryFile.wstring();
		size_t suffix = std::size(kDirectorySuffix) - 1;
		if (base_.size() < suffix || directory_.size() < 12 || U32(0) != 0x55AA1234) return Fail(error, Shown(directoryFile) + " isn't a VPK");
		base_.erase(base_.size() - suffix);
		uint32_t version = U32(4), treeSize = U32(8);
		size_t at = version == 2 ? 28 : 12;
		if (at > directory_.size() || treeSize > directory_.size() - at) return Fail(error, "the VPK directory is cut short");
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
					if (entry.preloadBytes > directory_.size() - at) return Fail(error, "the VPK directory is cut short");
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
		// (Open checked that the preloaded bytes are inside the directory)
		out.assign(directory_.begin() + entry.preloadAt, directory_.begin() + entry.preloadAt + entry.preloadBytes);
		if (entry.length == 0) return true;
		uint64_t end = static_cast<uint64_t>(entry.offset) + entry.length;
		if (entry.archive == 0x7FFF) {
			// (in the directory file itself, after the directory)
			if (dataStart_ + end > directory_.size()) return Fail(error, path + " runs past the archive");
			const char *data = &directory_[dataStart_ + entry.offset];
			out.insert(out.end(), data, data + entry.length);
			return true;
		}
		wchar_t number[16];
		swprintf(number, std::size(number), L"_%03u.vpk", static_cast<unsigned>(entry.archive));
		fs::path file = base_ + number;
		std::error_code ec;
		uintmax_t size = fs::file_size(file, ec);
		if (ec) return Fail(error, "can't open " + Shown(file));
		if (end > size) return Fail(error, path + " runs past " + Shown(file));
		std::ifstream in(file, std::ios::binary);
		in.seekg(entry.offset);
		size_t before = out.size();
		out.resize(before + entry.length);
		if (!in.read(out.data() + before, entry.length)) return Fail(error, "can't read " + path + " from " + Shown(file));
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
	std::wstring base_;  // the directory file's path without "_dir.vpk": the numbered files' names begin with it
	size_t dataStart_ = 0;
	std::map<std::string, Entry> entries_;
};

// ---- the model file (studiohdr_t, version 49) ----

constexpr size_t kName = 12, kNameSize = 64, kLength = 76, kNumTextures = 204, kTextureIndex = 208, kNumCdTextures = 212, kCdTextureIndex = 216,
                 kHeader2Index = 400, kHeader2Name = 0x14;
constexpr size_t kTextureSize = 64;

// Whether `size` bytes at `at` are inside the file. (`at` is often a sum of offsets read from the file: it may be
// anything, so nothing is added to it here that could wrap around.)
bool Inside(const std::vector<char> &data, size_t at, size_t size) { return at <= data.size() && data.size() - at >= size; }

// Whether a table of `count` records of `size` bytes at `at` is inside the file
bool Holds(const std::vector<char> &data, size_t at, int32_t count, size_t size) {
	return count >= 0 && at <= data.size() && static_cast<uint64_t>(count) * size <= data.size() - at;
}

int32_t I32(const std::vector<char> &data, size_t at) {
	int32_t v = 0;
	if (Inside(data, at, 4)) memcpy(&v, &data[at], 4);
	return v;
}

std::string CString(const std::vector<char> &data, size_t at) {
	std::string out;
	while (at < data.size() && data[at]) out += data[at++];
	return out;
}

// A material's name becomes a file's name under the plugin's material folder, and it is the model file's word:
// plain characters only, and no way out of that folder
bool PlainMaterialName(const std::string &name) {
	if (name.empty() || name.size() > 200 || name.find("..") != std::string::npos || name[0] == '/' || name[0] == '\\') return false;
	for (unsigned char c : name)
		if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != '/' && c != '\\' && c != ' ') return false;
	return true;
}

// Points every material folder of the model at the plugin's, and gives the copy its own name inside: the game
// finds a model's vertex and mesh files by that name, and a mod may have replaced the original's (a hat mod does),
// which the copy's files wouldn't match. Collects the material names the model uses.
bool Patch(std::vector<char> &mdl, const std::string &ownName, std::vector<std::string> &materials, std::string &error) {
	if (mdl.size() < 408 || memcmp(mdl.data(), "IDST", 4) != 0 || I32(mdl, 4) != 49) {
		error = "the model file isn't the kind this plugin knows (version 49)";
		return false;
	}
	// (the two lists' places and lengths are the file's word: inside the file, or the model isn't used)
	int32_t textures = I32(mdl, kNumTextures), textureIndex = I32(mdl, kTextureIndex);
	int32_t folders = I32(mdl, kNumCdTextures), folderIndex = I32(mdl, kCdTextureIndex);
	if (textureIndex < 0 || folderIndex < 0 || !Holds(mdl, static_cast<size_t>(textureIndex), textures, kTextureSize) ||
	    !Holds(mdl, static_cast<size_t>(folderIndex), folders, 4)) {
		error = "the model file's material lists run past its end";
		return false;
	}
	for (int32_t i = 0; i < textures; i++) {
		size_t at = static_cast<size_t>(textureIndex) + static_cast<size_t>(i) * kTextureSize;
		materials.push_back(CString(mdl, at + static_cast<size_t>(I32(mdl, at))));
	}
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
constexpr size_t kVtxLodHeader = 12, kVtxMeshHeader = 9, kVtxStripGroupHeader = 33, kVtxStripHeader = 35, kVtxVertex = 9;
constexpr size_t kMdlNumBodyParts = 232, kMdlBodyPartIndex = 236, kMdlBodyPart = 16, kMdlModel = 148, kMdlMesh = 116;
constexpr size_t kVvdNumVertices = 16, kVvdNumFixups = 48, kVvdFixupIndex = 52, kVvdVertexIndex = 56, kVvdVertex = 48, kVvdPosition = 16;

struct Vec3f {
	float x = 0, y = 0, z = 0;
};

float Read32f(const std::vector<char> &data, size_t at) {
	float v = 0;
	if (Inside(data, at, 4)) memcpy(&v, &data[at], 4);
	return v;
}

uint16_t U16(const std::vector<char> &data, size_t at) {
	uint16_t v = 0;
	if (Inside(data, at, 2)) memcpy(&v, &data[at], 2);
	return v;
}

void Put32(std::vector<char> &data, size_t at, int32_t v) {
	if (Inside(data, at, 4)) memcpy(&data[at], &v, 4);
}

// The vertex positions in the order the mesh file numbers them: the vertex file's "fixups" lay the vertices
// out level of detail by level of detail (at the root level every fixup is taken, in order)
std::vector<Vec3f> VertexPositions(const std::vector<char> &vvd) {
	std::vector<Vec3f> out;
	int32_t fixups = I32(vvd, kVvdNumFixups), fixupAt = I32(vvd, kVvdFixupIndex), verticesAt = I32(vvd, kVvdVertexIndex);
	// (no file holds more vertices than fit in it: counts beyond that are a damaged file's, and end the list)
	const size_t most = vvd.size() / kVvdVertex;
	auto position = [&](int32_t index) {
		size_t at = static_cast<size_t>(verticesAt) + static_cast<size_t>(index) * kVvdVertex + kVvdPosition;
		return Vec3f{Read32f(vvd, at), Read32f(vvd, at + 4), Read32f(vvd, at + 8)};
	};
	if (fixups == 0) {
		int32_t count = I32(vvd, kVvdNumVertices);
		for (int32_t i = 0; i < count && out.size() < most; i++) out.push_back(position(i));
		return out;
	}
	if (fixupAt < 0 || !Holds(vvd, static_cast<size_t>(fixupAt), fixups, 12)) return out;
	for (int32_t f = 0; f < fixups; f++) {
		size_t at = static_cast<size_t>(fixupAt) + static_cast<size_t>(f) * 12;
		int32_t source = I32(vvd, at + 4), count = I32(vvd, at + 8);
		for (int32_t i = 0; i < count && out.size() < most; i++) out.push_back(position(source + i));
	}
	return out;
}

float LongestEdge(const Vec3f &a, const Vec3f &b, const Vec3f &c) {
	auto length = [](const Vec3f &p, const Vec3f &q) {
		float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	};
	return std::max({length(a, b), length(b, c), length(c, a)});
}

// Makes the mesh file draw the model's coarsest level of detail whatever level the game picks: every level's
// header is pointed at the last level's meshes. With `minEdge` above zero the coarsest meshes are thinned too:
// every triangle whose longest edge is shorter is dropped (the lines left are the model's larger shapes; each
// strip keeps at least its largest triangle, since an empty strip crashes the game). Positions come from the
// vertex file, the meshes' material and vertex ranges from the model file.
//
// Every count and offset is the file's word. Reads and writes outside the file do nothing (I32, U16, Put32), and
// each table is checked to lie inside the file before it is walked, so a damaged file ends in an error.
bool CoarsestMesh(std::vector<char> &vtx, const std::vector<char> &mdl, const std::vector<char> &vvd, float minEdge, std::string &error) {
	if (vtx.size() < 36 || I32(vtx, 0) != 7) {
		error = "the mesh file isn't the kind this plugin knows (version 7)";
		return false;
	}
	auto damaged = [&error] {
		error = "the mesh file is cut short or damaged";
		return false;
	};
	std::vector<Vec3f> positions = minEdge > 0 ? VertexPositions(vvd) : std::vector<Vec3f>{};
	int32_t bodyParts = I32(vtx, kVtxNumBodyParts), bodyPartAt = I32(vtx, kVtxBodyPartIndex);
	int32_t mdlBodyPartAt = I32(mdl, kMdlBodyPartIndex);
	if (I32(mdl, kMdlNumBodyParts) != bodyParts) {
		error = "the model and mesh files disagree on the body parts";
		return false;
	}
	if (bodyPartAt < 0 || !Holds(vtx, static_cast<size_t>(bodyPartAt), bodyParts, 8)) return damaged();
	for (int32_t part = 0; part < bodyParts; part++) {
		size_t partAt = static_cast<size_t>(bodyPartAt) + static_cast<size_t>(part) * 8;
		size_t mdlPartAt = static_cast<size_t>(mdlBodyPartAt) + static_cast<size_t>(part) * kMdlBodyPart;
		int32_t models = I32(vtx, partAt), modelAt = I32(vtx, partAt + 4);
		int32_t mdlModelAt = I32(mdl, mdlPartAt + 12);
		if (!Holds(vtx, partAt + static_cast<size_t>(modelAt), models, 8)) return damaged();
		for (int32_t model = 0; model < models; model++) {
			size_t at = partAt + static_cast<size_t>(modelAt) + static_cast<size_t>(model) * 8;
			size_t mdlAt = mdlPartAt + static_cast<size_t>(mdlModelAt) + static_cast<size_t>(model) * kMdlModel;
			int32_t lods = I32(vtx, at), lodAt = I32(vtx, at + 4);
			if (lods < 1) continue;
			if (!Holds(vtx, at + static_cast<size_t>(lodAt), lods, kVtxLodHeader)) return damaged();
			size_t lastAt = at + static_cast<size_t>(lodAt) + static_cast<size_t>(lods - 1) * kVtxLodHeader;
			int32_t lastMeshes = I32(vtx, lastAt), lastMeshAt = I32(vtx, lastAt + 4);
			for (int32_t lod = 0; lod < lods - 1; lod++) {
				size_t headerAt = at + static_cast<size_t>(lodAt) + static_cast<size_t>(lod) * kVtxLodHeader;
				Put32(vtx, headerAt, lastMeshes);
				Put32(vtx, headerAt + 4, static_cast<int32_t>(lastAt + static_cast<size_t>(lastMeshAt)) - static_cast<int32_t>(headerAt));
			}
			if (minEdge <= 0 || lods < 2) continue;  // (a model with one level is left whole: all of its triangles are small)
			int32_t mdlMeshAt = I32(mdl, mdlAt + 76);
			size_t modelVertex = static_cast<size_t>(I32(mdl, mdlAt + 84)) / kVvdVertex;
			if (!Holds(vtx, lastAt + static_cast<size_t>(lastMeshAt), lastMeshes, kVtxMeshHeader)) return damaged();
			for (int32_t mesh = 0; mesh < lastMeshes; mesh++) {
				size_t meshAt = lastAt + static_cast<size_t>(lastMeshAt) + static_cast<size_t>(mesh) * kVtxMeshHeader;
				size_t meshVertex = modelVertex + static_cast<size_t>(I32(mdl, mdlAt + static_cast<size_t>(mdlMeshAt) + static_cast<size_t>(mesh) * kMdlMesh + 12));
				int32_t groups = I32(vtx, meshAt), groupAt = I32(vtx, meshAt + 4);
				if (!Holds(vtx, meshAt + static_cast<size_t>(groupAt), groups, kVtxStripGroupHeader)) return damaged();
				for (int32_t group = 0; group < groups; group++) {
					size_t gAt = meshAt + static_cast<size_t>(groupAt) + static_cast<size_t>(group) * kVtxStripGroupHeader;
					size_t verticesAt = gAt + static_cast<size_t>(I32(vtx, gAt + 4)), indicesAt = gAt + static_cast<size_t>(I32(vtx, gAt + 12));
					int32_t indices = I32(vtx, gAt + 8), strips = I32(vtx, gAt + 16), stripAt = I32(vtx, gAt + 20);
					if (!Holds(vtx, indicesAt, indices, 2) || !Holds(vtx, gAt + static_cast<size_t>(stripAt), strips, kVtxStripHeader)) return damaged();
					auto positionOf = [&](uint16_t local) {
						size_t index = meshVertex + U16(vtx, verticesAt + static_cast<size_t>(local) * kVtxVertex + 4);
						return index < positions.size() ? positions[index] : Vec3f{};
					};
					std::vector<uint16_t> old(static_cast<size_t>(indices));
					for (int32_t i = 0; i < indices; i++) old[static_cast<size_t>(i)] = U16(vtx, indicesAt + static_cast<size_t>(i) * 2);
					// (a strip is a run of the group's triangle list with its own bones: each is thinned in place and
					// the runs packed together again, in the order they came)
					std::vector<std::pair<int32_t, size_t>> runs;  // (first index, strip header)
					for (int32_t strip = 0; strip < strips; strip++) {
						size_t sAt = gAt + static_cast<size_t>(stripAt) + static_cast<size_t>(strip) * kVtxStripHeader;
						runs.push_back({I32(vtx, sAt + 4), sAt});
					}
					std::sort(runs.begin(), runs.end());
					std::vector<uint16_t> kept;
					for (const auto &run : runs) {
						int32_t first = run.first, count = I32(vtx, run.second);
						// (a strip is a part of the group's list. The thinned list must not have grown past where the
						// strip began, as it would with strips that share triangles: it is written back over the old one)
						if (first < 0 || count < 0 || static_cast<int64_t>(first) + count > indices) return damaged();
						if (static_cast<size_t>(first) < kept.size()) return damaged();
						size_t start = kept.size();
						float biggest = -1;
						size_t biggestAt = 0;
						for (int32_t t = first; t + 2 < first + count && t + 2 < indices; t += 3) {
							size_t i = static_cast<size_t>(t);
							float longest = LongestEdge(positionOf(old[i]), positionOf(old[i + 1]), positionOf(old[i + 2]));
							if (longest > biggest) {
								biggest = longest;
								biggestAt = i;
							}
							if (longest >= minEdge) kept.insert(kept.end(), {old[i], old[i + 1], old[i + 2]});
						}
						if (kept.size() == start && biggest >= 0) kept.insert(kept.end(), {old[biggestAt], old[biggestAt + 1], old[biggestAt + 2]});
						Put32(vtx, run.second, static_cast<int32_t>(kept.size() - start));
						Put32(vtx, run.second + 4, static_cast<int32_t>(start));
					}
					for (size_t i = 0; i < kept.size(); i++) memcpy(&vtx[indicesAt + i * 2], &kept[i], 2);
					Put32(vtx, gAt + 8, static_cast<int32_t>(kept.size()));
				}
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
		error = "can't write " + Shown(path);
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

std::string CopyName(const PlayerModel &model, Detail detail) {
	return std::string(detail == kBasic ? "ghostbasic_" : detail == kCoarse ? "ghostlow_" : "ghost_") + model.key;
}

// Reads a model's three files from the first archive that holds the model (all three from the same one: they
// belong together)
bool ReadModel(const PlayerModel &model, std::vector<char> &mdl, std::vector<char> &vvd, std::vector<char> &vtx, std::string &vtxExtension,
               std::string &error) {
	std::vector<fs::path> files = ArchiveFiles();
	if (files.empty()) {
		error = "no pak01_dir.vpk near " + Shown(GameFolder());
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
		error = std::string(model.source) + " has no mesh file in " + Shown(file);
		return false;
	}
	error = std::string(model.source) + ".mdl isn't in any of the game's archives";
	return false;
}

// Makes one model's copy (and the materials), unless it is already there
bool Make(const PlayerModel &model, Detail detail, std::string &error) {
	fs::path folder = GameFolder() / "models" / "p2practice";
	std::string name = CopyName(model, detail);
	fs::path mdlFile = folder / (name + ".mdl"), vvdFile = folder / (name + ".vvd"), vtxFile = folder / (name + ".dx90.vtx");
	std::error_code ec;
	if (fs::is_regular_file(mdlFile, ec) && fs::is_regular_file(vvdFile, ec) && fs::is_regular_file(vtxFile, ec)) return true;
	std::vector<char> mdl, vvd, vtx;
	std::string vtxExtension;
	if (!ReadModel(model, mdl, vvd, vtx, vtxExtension, error)) return false;
	std::vector<std::string> materials;
	if (!Patch(mdl, "p2practice/" + name + ".mdl", materials, error)) return false;
	// (the basic look drops the coarse mesh's triangles with no edge of 6 units: the robots keep their shapes,
	// and about a quarter of their lines)
	constexpr float kBasicEdge = 6;
	if (detail != kFull && !CoarsestMesh(vtx, mdl, vvd, detail == kBasic ? kBasicEdge : 0, error)) return false;
	// (the mesh file is written under the name this renderer looks for, whatever the source called it)
	if (!WriteFile(mdlFile, mdl.data(), mdl.size(), error) || !WriteFile(vvdFile, vvd.data(), vvd.size(), error) ||
	    !WriteFile(vtxFile, vtx.data(), vtx.size(), error))
		return false;
	for (const std::string &material : materials) {
		if (!PlainMaterialName(material)) continue;  // (no file is made for it: that part of the ghost has no material)
		fs::path file = GameFolder() / "materials" / "p2practice" / "ghost" / fs::u8path(material + ".vmt");
		if (!fs::is_regular_file(file, ec) && !WriteFile(file, kMaterial, strlen(kMaterial), error)) return false;
	}
	return true;
}

bool g_complained = false;

}  // namespace

std::string Wireframe(int team, Detail detail) {
	const PlayerModel &model = team == route::kTeamBlue ? kModels[0] : team == route::kTeamOrange ? kModels[1] : kModels[2];
	std::string error;
	bool ok = Make(model, detail, error);
	if (!ok) {
		if (!g_complained) engine::Warn("Wireframe ghosts aren't available: %s. Ghosts are shown as solid models.\n", error.c_str());
		g_complained = true;
		return "";
	}
	return "models/p2practice/" + CopyName(model, detail) + ".mdl";
}

}  // namespace ghostmodel
