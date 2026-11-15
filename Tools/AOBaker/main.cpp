/**
 * @file main.cpp
 * @brief AOBaker — bakes ambient occlusion into a glTF/GLB model's materials.
 *
 * For every material that has UV-mapped geometry the tool rasterizes the material's UV
 * triangles into an occlusion map (rays cast against the whole model, so other parts of the
 * model occlude too), then writes the model back out with that map attached as the
 * material's occlusionTexture. A .glb stays a single file (the PNGs go into its binary
 * chunk); a .gltf gets the PNGs written next to it and referenced by URI.
 */
#include "Bake.hpp"
#include "Gltf.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#include <cxxopts.hpp>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
	cxxopts::Options options("AOBaker", "Bake ambient occlusion maps into a glTF/GLB model");
	options.add_options()
		("i,input",   "Input GLTF/GLB file", cxxopts::value<std::string>())
		("o,output",  "Output model (default: <input>_ao.<ext> next to the input)", cxxopts::value<std::string>())
		("s,size",    "Occlusion map resolution (square)", cxxopts::value<int>()->default_value("512"))
		("n,samples", "Hemisphere rays per texel", cxxopts::value<int>()->default_value("32"))
		("r,radius",  "Ray length in model units; 0 = unbounded (full occlusion)", cxxopts::value<float>()->default_value("0"))
		("bias",      "Ray origin offset along the normal; 0 = automatic (0.01% of the model size)", cxxopts::value<float>()->default_value("0"))
		("blur",      "Box blur radius in texels", cxxopts::value<int>()->default_value("2"))
		("dilate",    "Padding passes into empty texels", cxxopts::value<int>()->default_value("6"))
		("power",     "Contrast: ao = pow(ao, power)", cxxopts::value<float>()->default_value("1.0"))
		("threads",   "Worker threads (0 = all cores)", cxxopts::value<int>()->default_value("0"))
		("flip-v",    "Treat the UV V axis as bottom-up")
		("dry-run",   "Bake and report, but do not write the model")
		("v,verbose", "Per-material progress and statistics")
		("h,help",    "Print help");

	cxxopts::ParseResult result;
	try {
		result = options.parse(argc, argv);
	}
	catch (const cxxopts::exceptions::exception& error) {
		std::fprintf(stderr, "%s\n\n%s\n", error.what(), options.help().c_str());
		return 1;
	}
	if (result.count("help") || !result.count("input")) {
		std::printf("%s\n", options.help().c_str());
		return result.count("help") ? 0 : 1;
	}

	const std::string inputPath = result["input"].as<std::string>();
	fs::path input(inputPath);
	if (!fs::exists(input)) { std::fprintf(stderr, "Input does not exist: %s\n", inputPath.c_str()); return 1; }

	std::string outputPath;
	if (result.count("output")) {
		outputPath = result["output"].as<std::string>();
	}
	else {
		fs::path out = input;
		out.replace_filename(input.stem().string() + "_ao" + input.extension().string());
		outputPath = out.string();
	}

	aob::BakeSettings settings;
	settings.size = static_cast<std::uint32_t>(std::max(1, result["size"].as<int>()));
	settings.samples = static_cast<std::uint32_t>(std::max(1, result["samples"].as<int>()));
	settings.distance = std::max(0.0f, result["radius"].as<float>());
	settings.bias = std::max(0.0f, result["bias"].as<float>());
	settings.blur = std::max(0, result["blur"].as<int>());
	settings.dilate = std::max(0, result["dilate"].as<int>());
	settings.power = std::max(0.01f, result["power"].as<float>());
	settings.threads = result["threads"].as<int>();
	settings.flipV = result.count("flip-v") > 0;
	settings.verbose = result.count("verbose") > 0;

	std::printf("AOBaker: %s\n", inputPath.c_str());
	std::string error;

	aob::ModelGeometry model;
	if (!aob::loadGeometry(inputPath, model, error)) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	const glm::vec3 extent = model.boundsMax - model.boundsMin;
	std::printf("  %zu triangle(s), %u material(s), bounds %.3f x %.3f x %.3f\n",
		model.tris.size(), model.materialCount,
		static_cast<double>(extent.x), static_cast<double>(extent.y), static_cast<double>(extent.z));

	std::vector<aob::AoMap> maps;
	if (!aob::bakeAll(model, settings, maps, error)) {
		std::fprintf(stderr, "  bake failed: %s\n", error.c_str());
		return 1;
	}

	if (result.count("dry-run")) {
		std::printf("  dry run: %zu map(s) baked, model not written\n", maps.size());
		return 0;
	}
	if (!aob::writeModelWithAo(inputPath, outputPath, maps, error)) {
		std::fprintf(stderr, "  write failed: %s\n", error.c_str());
		return 1;
	}
	std::printf("  wrote %s\n", outputPath.c_str());
	return 0;
}
