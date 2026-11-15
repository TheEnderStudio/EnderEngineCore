#include "Gltf.hpp"
#include "Json.hpp"
#include "Png.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

namespace aob {

namespace {

namespace fs = std::filesystem;

std::vector<std::uint8_t> readFile(const std::string& path, std::string& err) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { err = "cannot open " + path; return {}; }
	f.seekg(0, std::ios::end);
	const std::streamoff size = f.tellg();
	f.seekg(0, std::ios::beg);
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	if (size > 0) f.read(reinterpret_cast<char*>(bytes.data()), size);
	if (!f) { err = "cannot read " + path; return {}; }
	return bytes;
}

bool writeFile(const std::string& path, const void* data, std::size_t size, std::string& err) {
	std::ofstream f(path, std::ios::binary);
	if (!f) { err = "cannot write " + path; return false; }
	if (size) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	if (!f) { err = "failed writing " + path; return false; }
	return true;
}

std::uint32_t readU32(const std::uint8_t* p) {
	return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
void appendU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
	v.push_back(std::uint8_t(x & 0xFF));
	v.push_back(std::uint8_t((x >> 8) & 0xFF));
	v.push_back(std::uint8_t((x >> 16) & 0xFF));
	v.push_back(std::uint8_t((x >> 24) & 0xFF));
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

glm::mat4 nodeLocalMatrix(const fastgltf::Node& node) {
	if (const auto* m = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
		glm::mat4 out(1.0f);
		for (int c = 0; c < 4; ++c) {
			for (int r = 0; r < 4; ++r) out[c][r] = (*m)[c][r];
		}
		return out;
	}
	const auto& trs = std::get<fastgltf::TRS>(node.transform);
	const glm::quat rotation(trs.rotation.w(), trs.rotation.x(), trs.rotation.y(), trs.rotation.z());
	glm::mat4 out = glm::translate(glm::mat4(1.0f), glm::vec3(trs.translation.x(), trs.translation.y(), trs.translation.z()));
	out *= glm::mat4_cast(rotation);
	out = glm::scale(out, glm::vec3(trs.scale.x(), trs.scale.y(), trs.scale.z()));
	return out;
}

/// @brief Reads one fvec3/fvec2 attribute, or leaves @p dst untouched when absent.
template <typename T>
bool readVecAttribute(const fastgltf::Asset& asset, const fastgltf::Primitive& prim, const char* name,
	std::vector<T>& dst, std::size_t count) {
	auto it = prim.findAttribute(name);
	if (it == prim.attributes.end()) return false;
	const auto& accessor = asset.accessors[it->accessorIndex];
	if (accessor.count < count) return false;
	dst.assign(count, T{});
	fastgltf::iterateAccessorWithIndex<T>(asset, accessor, [&](T value, std::size_t index) {
		if (index < count) dst[index] = value;
	});
	return true;
}


} // namespace

bool loadGeometry(const std::string& path, ModelGeometry& out, std::string& err) {
	// Enable every extension bit: the baker only reads geometry, and it writes the model back
	// by patching the *original* JSON text, so extension data is carried through verbatim.
	// Without this, a model that requires any extension (KHR_texture_transform on the Wall,
	// Draco, meshopt, ...) fails to parse at all. fastgltf has no "all" enumerator, so flip
	// every bit; unknown bits are simply never consulted.
	const auto allExtensions = static_cast<fastgltf::Extensions>(~static_cast<std::uint64_t>(0));
	fastgltf::Parser parser(allExtensions);
	auto data = fastgltf::GltfDataBuffer::FromPath(path);
	if (data.error() != fastgltf::Error::None) {
		err = std::string("glTF load failed: ") + fastgltf::getErrorMessage(data.error()).data();
		return false;
	}
	auto asset = parser.loadGltf(data.get(), fs::path(path).parent_path(), fastgltf::Options::LoadExternalBuffers);
	if (asset.error() != fastgltf::Error::None) {
		err = std::string("glTF parse failed: ") + fastgltf::getErrorMessage(asset.error()).data();
		return false;
	}
	const fastgltf::Asset& model = asset.get();
	out.materialCount = static_cast<std::uint32_t>(model.materials.size());
	out.materialNames.reserve(model.materials.size());
	for (const auto& material : model.materials) {
		std::string name = material.name.empty() ? std::string() : std::string(material.name.c_str());
		out.materialNames.push_back(name);
	}

	// Walk the scene graph, accumulating node transforms. Every node is visited once even
	// when it is not reachable from the default scene, so instanced meshes are baked too.
	std::vector<bool> visited(model.nodes.size(), false);

	struct StackEntry { std::size_t node; glm::mat4 parentWorld; };

	auto emitPrimitive = [&](const fastgltf::Primitive& prim, const glm::mat4& world) {
		const auto posIt = prim.findAttribute("POSITION");
		if (posIt == prim.attributes.end()) return;
		const auto& posAcc = model.accessors[posIt->accessorIndex];
		const std::size_t vertexCount = posAcc.count;
		if (vertexCount == 0) return;

		std::vector<fastgltf::math::fvec3> positions;
		if (!readVecAttribute(model, prim, "POSITION", positions, vertexCount)) return;

		std::vector<fastgltf::math::fvec3> normals;
		const bool hasNormals = readVecAttribute(model, prim, "NORMAL", normals, vertexCount);
		std::vector<fastgltf::math::fvec2> uvs;
		const bool hasUvs = readVecAttribute(model, prim, "TEXCOORD_0", uvs, vertexCount);

		const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(world)));
		std::vector<glm::vec3> worldPos(vertexCount), worldNrm(vertexCount);
		for (std::size_t i = 0; i < vertexCount; ++i) {
			const auto& p = positions[i];
			worldPos[i] = glm::vec3(world * glm::vec4(p.x(), p.y(), p.z(), 1.0f));
			if (hasNormals) {
				const auto& n = normals[i];
				const glm::vec3 nw = normalMatrix * glm::vec3(n.x(), n.y(), n.z());
				const float len2 = glm::dot(nw, nw);
				worldNrm[i] = len2 > 1e-12f ? nw / std::sqrt(len2) : glm::vec3(0.0f, 1.0f, 0.0f);
			}
			else {
				worldNrm[i] = glm::vec3(0.0f);
			}
		}

		std::vector<std::uint32_t> indices;
		if (prim.indicesAccessor.has_value()) {
			const auto& indexAcc = model.accessors[prim.indicesAccessor.value()];
			indices.reserve(indexAcc.count);
			fastgltf::iterateAccessor<std::uint32_t>(model, indexAcc, [&](std::uint32_t value) { indices.push_back(value); });
		}
		else {
			indices.resize(vertexCount);
			for (std::size_t i = 0; i < vertexCount; ++i) indices[i] = static_cast<std::uint32_t>(i);
		}
		if (indices.size() < 3) return;

		const std::uint32_t material = prim.materialIndex.has_value()
			? static_cast<std::uint32_t>(prim.materialIndex.value())
			: UINT32_MAX;

		auto emitTriangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
			if (a >= vertexCount || b >= vertexCount || c >= vertexCount) return;
			Tri tri;
			tri.material = material;
			tri.hasUv = hasUvs;
			const std::uint32_t idx[3] = { a, b, c };
			for (int k = 0; k < 3; ++k) {
				tri.p[k] = worldPos[idx[k]];
				tri.n[k] = worldNrm[idx[k]];
				if (hasUvs) {
					const auto& uv = uvs[idx[k]];
					tri.uv[k] = glm::vec2(uv.x(), uv.y());
				}
			}
			// A missing normal attribute falls back to the geometric normal (flat shading),
			// which is all an occlusion bake needs.
			for (int k = 0; k < 3; ++k) {
				if (glm::dot(tri.n[k], tri.n[k]) > 1e-8f) continue;
				const glm::vec3 face = glm::cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]);
				const float len2 = glm::dot(face, face);
				const glm::vec3 n = len2 > 1e-16f ? face / std::sqrt(len2) : glm::vec3(0.0f, 1.0f, 0.0f);
				for (int j = 0; j < 3; ++j) tri.n[j] = n;
				break;
			}
			out.tris.push_back(tri);
		};

		switch (prim.type) {
		case fastgltf::PrimitiveType::Triangles:
		case fastgltf::PrimitiveType::TriangleStrip:
		case fastgltf::PrimitiveType::TriangleFan:
			break;
		default:
			return; // points / lines contribute no occlusion surface
		}

		if (prim.type == fastgltf::PrimitiveType::Triangles) {
			for (std::size_t i = 0; i + 2 < indices.size(); i += 3) emitTriangle(indices[i], indices[i + 1], indices[i + 2]);
		}
		else if (prim.type == fastgltf::PrimitiveType::TriangleStrip) {
			for (std::size_t i = 0; i + 2 < indices.size(); ++i) {
				if ((i & 1) == 0) emitTriangle(indices[i], indices[i + 1], indices[i + 2]);
				else             emitTriangle(indices[i + 1], indices[i], indices[i + 2]);
			}
		}
		else {
			for (std::size_t i = 1; i + 1 < indices.size(); ++i) emitTriangle(indices[0], indices[i], indices[i + 1]);
		}
	};

	std::vector<StackEntry> stack;
	auto pushNode = [&](std::size_t index, const glm::mat4& parentWorld) {
		if (index >= model.nodes.size()) return;
		stack.push_back({ index, parentWorld });
	};
	if (!model.scenes.empty()) {
		for (const std::size_t root : model.scenes[model.defaultScene.has_value() ? model.defaultScene.value() : 0].nodeIndices) pushNode(root, glm::mat4(1.0f));
	}
	else {
		// No scene: treat every node as a root (the model is then whatever the nodes say).
		for (std::size_t i = 0; i < model.nodes.size(); ++i) pushNode(i, glm::mat4(1.0f));
	}

	while (!stack.empty()) {
		const StackEntry entry = stack.back();
		stack.pop_back();
		if (visited[entry.node]) continue;
		visited[entry.node] = true;
		const fastgltf::Node& node = model.nodes[entry.node];
		const glm::mat4 world = entry.parentWorld * nodeLocalMatrix(node);
		if (node.meshIndex.has_value()) {
			const fastgltf::Mesh& mesh = model.meshes[node.meshIndex.value()];
			for (const auto& prim : mesh.primitives) emitPrimitive(prim, world);
		}
		for (const std::size_t child : node.children) pushNode(child, world);
	}

	if (out.tris.empty()) { err = "the model has no triangle geometry"; return false; }

	out.boundsMin = glm::vec3(std::numeric_limits<float>::max());
	out.boundsMax = glm::vec3(-std::numeric_limits<float>::max());
	for (const Tri& tri : out.tris) {
		for (int k = 0; k < 3; ++k) {
			out.boundsMin = glm::min(out.boundsMin, tri.p[k]);
			out.boundsMax = glm::max(out.boundsMax, tri.p[k]);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Writing the model back
// ---------------------------------------------------------------------------

bool writeModelWithAo(const std::string& inputPath, const std::string& outputPath,
	const std::vector<AoMap>& maps, std::string& err) {
	if (maps.empty()) { err = "no AO maps to attach"; return false; }

	std::vector<std::uint8_t> file = readFile(inputPath, err);
	if (file.empty()) return false;

	const bool isGlb = file.size() >= 12 && file[0] == 'g' && file[1] == 'l' && file[2] == 'T' && file[3] == 'F';
	std::string jsonText;
	std::vector<std::uint8_t> binData;
	if (isGlb) {
		const std::uint32_t version = readU32(file.data() + 4);
		if (version != 2) { err = "not a glTF 2.0 binary"; return false; }
		const std::uint32_t total = readU32(file.data() + 8);
		if (total > file.size()) { err = "truncated GLB"; return false; }
		std::size_t offset = 12;
		bool haveJson = false;
		while (offset + 8 <= total) {
			const std::uint32_t chunkLength = readU32(file.data() + offset);
			const std::uint32_t chunkType = readU32(file.data() + offset + 4);
			const std::size_t chunkStart = offset + 8;
			if (chunkStart + chunkLength > file.size()) { err = "truncated GLB chunk"; return false; }
			if (chunkType == 0x4E4F534A) { // "JSON"
				jsonText.assign(reinterpret_cast<const char*>(file.data() + chunkStart), chunkLength);
				haveJson = true;
			}
			else if (chunkType == 0x004E4942) { // "BIN\0"
				binData.assign(file.begin() + static_cast<std::ptrdiff_t>(chunkStart),
					file.begin() + static_cast<std::ptrdiff_t>(chunkStart + chunkLength));
			}
			offset = chunkStart + chunkLength;
		}
		if (!haveJson) { err = "GLB has no JSON chunk"; return false; }
	}
	else {
		jsonText.assign(reinterpret_cast<const char*>(file.data()), file.size());
	}

	Json root;
	std::string parseError;
	if (!parseJson(jsonText, root, parseError)) { err = "cannot parse the glTF JSON: " + parseError; return false; }
	if (!root.isObject()) { err = "the glTF JSON root is not an object"; return false; }

	// Every lookup goes through the root each time on purpose: appending a top-level array
	// grows the root object's member vector, which invalidates any reference into it, so
	// holding Json& materials = root.array("materials") across the additions below would
	// dangle (it read an empty array and reported "material index out of range").
	const auto arrayOf = [&root](const char* name) -> Json& { return root.array(name); };

	// One shared linear/repeat sampler for every baked map.
	std::int64_t samplerIndex = -1;
	{
		Json& samplers = arrayOf("samplers");
		for (std::size_t i = 0; i < samplers.size(); ++i) {
			const Json* mag = samplers.at(i).find("magFilter");
			const Json* min = samplers.at(i).find("minFilter");
			const Json* wrapS = samplers.at(i).find("wrapS");
			if (mag && min && wrapS && mag->integer() == 9729 && min->integer() == 9987 && wrapS->integer() == 10497) {
				samplerIndex = static_cast<std::int64_t>(i);
				break;
			}
		}
		if (samplerIndex < 0) {
			Json sampler = Json::makeObject();
			sampler.set("magFilter", Json::makeInteger(9729));  // LINEAR
			sampler.set("minFilter", Json::makeInteger(9987));  // LINEAR_MIPMAP_LINEAR
			sampler.set("wrapS", Json::makeInteger(10497));     // REPEAT
			sampler.set("wrapT", Json::makeInteger(10497));
			samplerIndex = static_cast<std::int64_t>(samplers.size());
			samplers.push(std::move(sampler));
		}
	}

	const fs::path outPath(outputPath);
	const std::string outStem = outPath.stem().string();
	const fs::path outDir = outPath.has_parent_path() ? outPath.parent_path() : fs::path(".");

	for (const AoMap& map : maps) {
		if (map.rgb.empty() || map.width == 0 || map.height == 0) continue;
		if (map.material >= arrayOf("materials").size()) {
			err = "material index out of range while writing: " + std::to_string(map.material);
			return false;
		}

		std::vector<std::uint8_t> png = encodePng(map.width, map.height, map.rgb.data());
		if (png.empty()) { err = "PNG encoding failed"; return false; }

		Json image = Json::makeObject();
		if (isGlb) {
			while (binData.size() % 4 != 0) binData.push_back(0);
			const std::int64_t byteOffset = static_cast<std::int64_t>(binData.size());
			binData.insert(binData.end(), png.begin(), png.end());
			Json view = Json::makeObject();
			view.set("buffer", Json::makeInteger(0));
			view.set("byteOffset", Json::makeInteger(byteOffset));
			view.set("byteLength", Json::makeInteger(static_cast<std::int64_t>(png.size())));
			Json& views = arrayOf("bufferViews");
			const std::int64_t viewIndex = static_cast<std::int64_t>(views.size());
			views.push(std::move(view));
			image.set("bufferView", Json::makeInteger(viewIndex));
			image.set("mimeType", Json::makeString("image/png"));
		}
		else {
			const std::string fileName = outStem + "_ao" + std::to_string(map.material) + ".png";
			if (!writeFile((outDir / fileName).string(), png.data(), png.size(), err)) return false;
			image.set("uri", Json::makeString(fileName));
		}
		if (!map.name.empty()) image.set("name", Json::makeString(map.name));

		Json& images = arrayOf("images");
		const std::int64_t imageIndex = static_cast<std::int64_t>(images.size());
		images.push(std::move(image));

		Json texture = Json::makeObject();
		texture.set("sampler", Json::makeInteger(samplerIndex));
		texture.set("source", Json::makeInteger(imageIndex));
		Json& textures = arrayOf("textures");
		const std::int64_t textureIndex = static_cast<std::int64_t>(textures.size());
		textures.push(std::move(texture));

		Json& material = arrayOf("materials").at(map.material);
		if (!material.isObject()) { err = "materials[" + std::to_string(map.material) + "] is not an object"; return false; }
		Json occlusion = Json::makeObject();
		occlusion.set("index", Json::makeInteger(textureIndex));
		occlusion.set("texCoord", Json::makeInteger(0));
		material.set("occlusionTexture", std::move(occlusion));
	}

	if (isGlb) {
		// Rewrite buffer 0's length to include the appended PNGs.
		Json& buffers = arrayOf("buffers");
		if (buffers.size() == 0) {
			Json buffer = Json::makeObject();
			buffer.set("byteLength", Json::makeInteger(static_cast<std::int64_t>(binData.size())));
			buffers.push(std::move(buffer));
		}
		else {
			buffers.at(0).set("byteLength", Json::makeInteger(static_cast<std::int64_t>(binData.size())));
		}
	}

	std::string outputJson = writeJson(root);
	// GLB pads the JSON chunk with spaces and the binary chunk with zeros.
	std::vector<std::uint8_t> output;
	if (isGlb) {
		std::vector<std::uint8_t> jsonBytes(outputJson.begin(), outputJson.end());
		while (jsonBytes.size() % 4 != 0) jsonBytes.push_back(0x20);
		while (binData.size() % 4 != 0) binData.push_back(0x00);
		const std::uint32_t total = static_cast<std::uint32_t>(12 + 8 + jsonBytes.size() + (binData.empty() ? 0 : 8 + binData.size()));
		appendU32(output, 0x46546C67); // "glTF"
		appendU32(output, 2);
		appendU32(output, total);
		appendU32(output, static_cast<std::uint32_t>(jsonBytes.size()));
		appendU32(output, 0x4E4F534A);
		output.insert(output.end(), jsonBytes.begin(), jsonBytes.end());
		if (!binData.empty()) {
			appendU32(output, static_cast<std::uint32_t>(binData.size()));
			appendU32(output, 0x004E4942);
			output.insert(output.end(), binData.begin(), binData.end());
		}
	}
	else {
		output.assign(outputJson.begin(), outputJson.end());
	}
	return writeFile(outputPath, output.data(), output.size(), err);
}

} // namespace aob
