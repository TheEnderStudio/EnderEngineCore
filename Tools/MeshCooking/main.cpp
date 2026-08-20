/// MeshCooking — offline tool to pre-cook GLTF/GLB meshes into PhysX binary format.

#include <cstdio>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>

#include <cxxopts.hpp>

#define NOMINMAX
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultStreams.h>
#include <cooking/PxCooking.h>
#include <cooking/PxSDFDesc.h>

using namespace physx;

static bool collectAllMeshes(const fastgltf::Asset& asset,
                             std::vector<fastgltf::math::fvec3>& verts,
                             std::vector<std::uint32_t>& indices) {
	std::uint32_t base = 0;
	for (auto& mesh : asset.meshes) {
		for (auto& prim : mesh.primitives) {
			if (!prim.indicesAccessor.has_value()) continue;
			auto* posAttr = prim.findAttribute("POSITION");
			if (!posAttr) continue;
			auto& pAcc = asset.accessors[posAttr->accessorIndex];
			auto& iAcc = asset.accessors[prim.indicesAccessor.value()];
			for (size_t j = 0; j < pAcc.count; j++)
				verts.push_back(fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, pAcc, j));
			for (size_t j = 0; j < iAcc.count; j++)
				indices.push_back(base + fastgltf::getAccessorElement<std::uint32_t>(asset, iAcc, j));
			base += (std::uint32_t)pAcc.count;
		}
	}
	return !verts.empty();
}

static bool saveFile(const char* path, const void* data, size_t size) {
	std::ofstream f(path, std::ios::binary);
	if (!f) return false;
	f.write((const char*)data, size);
	return true;
}

int main(int argc, char** argv) {
	cxxopts::Options opts("MeshCooking", "Pre-cook GLTF/GLB models into PhysX collision meshes");
	opts.add_options()
		("i,input",   "Input GLTF/GLB file", cxxopts::value<std::string>())
		("o,output",  "Output file prefix", cxxopts::value<std::string>()->default_value("cooked"))
		("t,tri",     "Cook triangle mesh (.tri)")
		("c,conv",    "Cook convex hull (.conv)")
		("s,sdf",     "Cook SDF mesh (.sdf)")
		("r,res",     "SDF grid resolution", cxxopts::value<int>()->default_value("64"))
		("h,help",    "Print help");

	auto result = opts.parse(argc, argv);
	if (result.count("help") || !result.count("input")) {
		printf("%s\n", opts.help().c_str());
		return 1;
	}

	std::string inputPath = result["input"].as<std::string>();
	std::string prefix    = result["output"].as<std::string>();
	bool doTri  = result.count("tri");
	bool doConv = result.count("conv");
	bool doSDF  = result.count("sdf");
	int  sdfRes = result["res"].as<int>();
	if (!doTri && !doConv && !doSDF) doTri = doConv = true;

	// Load GLTF
	fastgltf::Parser parser;
	auto gd = fastgltf::GltfDataBuffer::FromPath(inputPath);
	if (gd.error() != fastgltf::Error::None) { fprintf(stderr, "Load failed: %s\n", fastgltf::getErrorMessage(gd.error()).data()); return 1; }
	auto asset = parser.loadGltf(gd.get(), std::filesystem::path(inputPath).parent_path(), fastgltf::Options::LoadExternalBuffers);
	if (asset.error() != fastgltf::Error::None) { fprintf(stderr, "Parse failed: %s\n", fastgltf::getErrorMessage(asset.error()).data()); return 1; }
	auto& model = asset.get();
	printf("Loaded: %zu meshes\n", model.meshes.size());

	std::vector<fastgltf::math::fvec3> allVerts;
	std::vector<std::uint32_t> allIndices;
	if (!collectAllMeshes(model, allVerts, allIndices)) { fprintf(stderr, "No mesh data\n"); return 1; }
	printf("Merged: %zu verts, %zu tris\n", allVerts.size(), allIndices.size() / 3);

	PxDefaultAllocator alc; PxDefaultErrorCallback ecb;
	PxFoundation* fnd = PxCreateFoundation(PX_PHYSICS_VERSION, alc, ecb);
	if (!fnd) { fprintf(stderr, "PxCreateFoundation failed\n"); return 1; }

	PxTolerancesScale scale; PxCookingParams params(scale);
	std::string base = prefix + "_" + std::filesystem::path(inputPath).filename().replace_extension("").string();

	// Triangle mesh
	if (doTri) {
		PxTriangleMeshDesc td;
		td.points.count = (PxU32)allVerts.size(); td.points.stride = sizeof(fastgltf::math::fvec3); td.points.data = allVerts.data();
		td.triangles.count = (PxU32)allIndices.size() / 3; td.triangles.stride = 3 * sizeof(uint32_t); td.triangles.data = allIndices.data();
		PxDefaultMemoryOutputStream os;
		if (PxCookTriangleMesh(params, td, os)) {
			saveFile((base + ".tri").c_str(), os.getData(), os.getSize());
			printf("  Triangle: %u verts %u tris -> %u B -> %s\n", td.points.count, td.triangles.count, os.getSize(), (base + ".tri").c_str());
		} else fprintf(stderr, "  Triangle cook failed\n");
	}

	// Convex hull
	if (doConv) {
		std::vector<fastgltf::math::fvec3> sub;
		if (allVerts.size() > 256) {
			size_t step = allVerts.size() / 256 + 1;
			for (size_t i = 0; i < allVerts.size(); i += step) sub.push_back(allVerts[i]);
			printf("  Convex: subsampled %zu -> %zu verts\n", allVerts.size(), sub.size());
		} else sub = allVerts;
		PxConvexMeshDesc cd;
		cd.points.count = (PxU32)sub.size(); cd.points.stride = sizeof(fastgltf::math::fvec3); cd.points.data = sub.data();
		cd.flags = PxConvexFlag::eCOMPUTE_CONVEX;
		PxDefaultMemoryOutputStream os;
		if (PxCookConvexMesh(params, cd, os)) {
			saveFile((base + ".conv").c_str(), os.getData(), os.getSize());
			printf("  Convex: %u verts -> %u B -> %s\n", cd.points.count, os.getSize(), (base + ".conv").c_str());
		} else fprintf(stderr, "  Convex cook failed\n");
	}

	// SDF mesh
	if (doSDF) {
		float mx = float(allVerts[0][0]), Mx = mx, my = float(allVerts[0][1]), My = my, mz = float(allVerts[0][2]), Mz = mz;
		for (auto& v : allVerts) {
			float vx = float(v[0]), vy = float(v[1]), vz = float(v[2]);
			if (vx < mx) mx = vx; if (vx > Mx) Mx = vx;
			if (vy < my) my = vy; if (vy > My) My = vy;
			if (vz < mz) mz = vz; if (vz > Mz) Mz = vz;
		}
		float e = std::max({ Mx - mx, My - my, Mz - mz }) * 0.55f;
		if (e < 0.001f) e = 1.0f;

		PxSDFDesc sdf;
		sdf.dims = { (PxU32)sdfRes, (PxU32)sdfRes, (PxU32)sdfRes };
		sdf.meshLower = PxVec3(mx - e * 0.1f, my - e * 0.1f, mz - e * 0.1f);
		sdf.spacing = (e * 2.2f) / sdfRes;
		sdf.subgridSize = 6;

		PxTriangleMeshDesc td;
		td.points.count = (PxU32)allVerts.size(); td.points.stride = sizeof(fastgltf::math::fvec3); td.points.data = allVerts.data();
		td.triangles.count = (PxU32)allIndices.size() / 3; td.triangles.stride = 3 * sizeof(uint32_t); td.triangles.data = allIndices.data();
		td.sdfDesc = &sdf;

		PxDefaultMemoryOutputStream os;
		if (PxCookTriangleMesh(params, td, os)) {
			saveFile((base + ".sdf").c_str(), os.getData(), os.getSize());
			printf("  SDF: %d^3 grid bound=%.1f -> %u B -> %s\n", sdfRes, e * 2, os.getSize(), (base + ".sdf").c_str());
		} else fprintf(stderr, "  SDF cook failed\n");
	}

	fnd->release();
	printf("Done.\n");
	return 0;
}
