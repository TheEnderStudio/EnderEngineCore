#include "Bake.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace aob {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// BVH over the whole model: any-hit occlusion queries
// ---------------------------------------------------------------------------

struct BvhTriangle {
	glm::vec3 v0{0.0f};
	glm::vec3 e1{0.0f};
	glm::vec3 e2{0.0f};
};

struct BvhNode {
	glm::vec3 boundsMin{0.0f};
	glm::vec3 boundsMax{0.0f};
	std::uint32_t first = 0;  ///< First triangle of a leaf.
	std::uint32_t count = 0;  ///< Triangles in a leaf; 0 marks an interior node.
	std::int32_t right = -1;  ///< Interior: right child. The left child is always node + 1.
};

struct Bvh {
	std::vector<BvhTriangle> tris;
	std::vector<BvhNode> nodes;

	/// @brief True when the segment origin..origin + dir*maxT hits any triangle.
	[[nodiscard]] bool occluded(const glm::vec3& origin, const glm::vec3& dir, float maxT) const {
		if (nodes.empty()) return false;
		const glm::vec3 safeDir(dir.x != 0.0f ? dir.x : 1e-20f,
			dir.y != 0.0f ? dir.y : 1e-20f,
			dir.z != 0.0f ? dir.z : 1e-20f);
		const glm::vec3 inv(1.0f / safeDir.x, 1.0f / safeDir.y, 1.0f / safeDir.z);
		std::int32_t stack[64];
		int top = 0;
		stack[top++] = 0;
		while (top > 0) {
			const std::int32_t index = stack[--top];
			const BvhNode& node = nodes[static_cast<std::size_t>(index)];
			{
				const glm::vec3 t0 = (node.boundsMin - origin) * inv;
				const glm::vec3 t1 = (node.boundsMax - origin) * inv;
				const glm::vec3 lo = glm::min(t0, t1);
				const glm::vec3 hi = glm::max(t0, t1);
				const float near = std::max(std::max(lo.x, lo.y), lo.z);
				const float far = std::min(std::min(hi.x, hi.y), hi.z);
				if (far < std::max(near, 0.0f) || near > maxT) continue;
			}
			if (node.count > 0) {
				for (std::uint32_t i = 0; i < node.count; ++i) {
					const BvhTriangle& tri = tris[node.first + i];
					// Möller-Trumbore, any-hit.
					const glm::vec3 p = glm::cross(dir, tri.e2);
					const float det = glm::dot(tri.e1, p);
					if (std::fabs(det) < 1e-12f) continue;
					const float invDet = 1.0f / det;
					const glm::vec3 tv = origin - tri.v0;
					const float u = glm::dot(tv, p) * invDet;
					if (u < 0.0f || u > 1.0f) continue;
					const glm::vec3 q = glm::cross(tv, tri.e1);
					const float v = glm::dot(dir, q) * invDet;
					if (v < 0.0f || u + v > 1.0f) continue;
					const float t = glm::dot(tri.e2, q) * invDet;
					if (t > 1e-6f && t < maxT) return true;
				}
				continue;
			}
			if (top + 2 <= 64) {
				stack[top++] = index + 1;
				stack[top++] = node.right;
			}
		}
		return false;
	}
};

struct BvhBuildTriangle {
	glm::vec3 v0{0.0f};
	glm::vec3 e1{0.0f};
	glm::vec3 e2{0.0f};
	glm::vec3 centroid{0.0f};
	glm::vec3 boundsMin{0.0f};
	glm::vec3 boundsMax{0.0f};
};

class BvhBuilder {
public:
	explicit BvhBuilder(std::vector<BvhBuildTriangle> tris) : m_tris(std::move(tris)) {}

	Bvh build() {
		Bvh bvh;
		if (m_tris.empty()) return bvh;
		bvh.nodes.reserve(m_tris.size() * 2);
		buildNode(bvh, 0, static_cast<std::uint32_t>(m_tris.size()));
		bvh.tris.reserve(m_tris.size());
		for (const BvhBuildTriangle& tri : m_tris) bvh.tris.push_back({ tri.v0, tri.e1, tri.e2 });
		return bvh;
	}

private:
	std::int32_t buildNode(Bvh& bvh, std::uint32_t first, std::uint32_t count) {
		const std::int32_t index = static_cast<std::int32_t>(bvh.nodes.size());
		bvh.nodes.emplace_back();

		glm::vec3 boundsMin(std::numeric_limits<float>::max());
		glm::vec3 boundsMax(-std::numeric_limits<float>::max());
		for (std::uint32_t i = 0; i < count; ++i) {
			const BvhBuildTriangle& tri = m_tris[first + i];
			boundsMin = glm::min(boundsMin, tri.boundsMin);
			boundsMax = glm::max(boundsMax, tri.boundsMax);
		}
		bvh.nodes[static_cast<std::size_t>(index)].boundsMin = boundsMin;
		bvh.nodes[static_cast<std::size_t>(index)].boundsMax = boundsMax;

		if (count <= 4) {
			bvh.nodes[static_cast<std::size_t>(index)].first = first;
			bvh.nodes[static_cast<std::size_t>(index)].count = count;
			return index;
		}

		// Split at the median of the centroids along the widest axis.
		const glm::vec3 extent = boundsMax - boundsMin;
		int axis = 0;
		if (extent.y > extent.x) axis = 1;
		if (extent.z > extent[axis]) axis = 2;
		const std::uint32_t middle = count / 2;
		std::nth_element(m_tris.begin() + first, m_tris.begin() + first + middle, m_tris.begin() + first + count,
			[axis](const BvhBuildTriangle& a, const BvhBuildTriangle& b) { return a.centroid[axis] < b.centroid[axis]; });

		buildNode(bvh, first, middle);
		const std::int32_t right = buildNode(bvh, first + middle, count - middle);
		bvh.nodes[static_cast<std::size_t>(index)].right = right;
		return index;
	}

	std::vector<BvhBuildTriangle> m_tris;
};

/// @brief Build the occluder BVH from every triangle of the model.
Bvh buildModelBvh(const ModelGeometry& model) {
	std::vector<BvhBuildTriangle> tris;
	tris.reserve(model.tris.size());
	for (const Tri& tri : model.tris) {
		BvhBuildTriangle bt;
		bt.v0 = tri.p[0];
		bt.e1 = tri.p[1] - tri.p[0];
		bt.e2 = tri.p[2] - tri.p[0];
		if (glm::dot(glm::cross(bt.e1, bt.e2), glm::cross(bt.e1, bt.e2)) < 1e-20f) continue; // degenerate
		bt.centroid = (tri.p[0] + tri.p[1] + tri.p[2]) / 3.0f;
		bt.boundsMin = glm::min(tri.p[0], glm::min(tri.p[1], tri.p[2]));
		bt.boundsMax = glm::max(tri.p[0], glm::max(tri.p[1], tri.p[2]));
		tris.push_back(bt);
	}
	return BvhBuilder(std::move(tris)).build();
}

// ---------------------------------------------------------------------------
// Sampling helpers
// ---------------------------------------------------------------------------

void buildOnb(const glm::vec3& n, glm::vec3& tangent, glm::vec3& bitangent) {
	const glm::vec3 up = std::fabs(n.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	tangent = glm::normalize(glm::cross(up, n));
	bitangent = glm::cross(n, tangent);
}

/// @brief Van der Corput radical inverse in base 2 (Hammersley's second coordinate).
float radicalInverse(std::uint32_t bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

std::uint32_t hash32(std::uint32_t x) {
	x ^= x >> 16; x *= 0x7FEB352Du;
	x ^= x >> 15; x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/// @brief Cosine-weighted hemisphere direction around @p normal for sample @p index.
glm::vec3 sampleCosineHemisphere(const glm::vec3& tangent, const glm::vec3& bitangent, const glm::vec3& normal,
	float u1, float u2) {
	const float r = std::sqrt(u1);
	const float phi = 2.0f * kPi * u2;
	const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
	return glm::normalize(tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) + normal * z);
}

/// @brief One texel's surface: the average position and normal of every triangle touching it.
struct SurfaceTexel {
	glm::vec3 position{0.0f};
	glm::vec3 normal{0.0f};
	float weight = 0.0f;
};

float edgeFunction(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// ---------------------------------------------------------------------------
// One material
// ---------------------------------------------------------------------------

/// @brief Rasterizes the material's UV triangles into per-texel positions/normals.
void rasterizeUv(const std::vector<const Tri*>& tris, std::uint32_t size, bool flipV,
	std::vector<SurfaceTexel>& surface, std::size_t& coveredTexels) {
	coveredTexels = 0;
	const float invSize = 1.0f / static_cast<float>(size);
	for (const Tri* tri : tris) {
		glm::vec2 uv[3];
		for (int k = 0; k < 3; ++k) {
			uv[k] = tri->uv[k];
			if (flipV) uv[k].y = 1.0f - uv[k].y;
		}
		const float area = edgeFunction(uv[0], uv[1], uv[2]);
		if (std::fabs(area) < 1e-12f) continue; // degenerate in UV space

		glm::vec2 lo = glm::min(uv[0], glm::min(uv[1], uv[2]));
		glm::vec2 hi = glm::max(uv[0], glm::max(uv[1], uv[2]));
		// Clamp to the map and expand by one texel so a triangle whose edge runs exactly on
		// a texel boundary still covers that texel.
		int x0 = static_cast<int>(std::floor(lo.x * size)) - 1;
		int y0 = static_cast<int>(std::floor(lo.y * size)) - 1;
		int x1 = static_cast<int>(std::ceil(hi.x * size)) + 1;
		int y1 = static_cast<int>(std::ceil(hi.y * size)) + 1;
		x0 = std::max(x0, 0); y0 = std::max(y0, 0);
		x1 = std::min(x1, static_cast<int>(size) - 1);
		y1 = std::min(y1, static_cast<int>(size) - 1);

		const float invArea = 1.0f / area;
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const glm::vec2 p((static_cast<float>(x) + 0.5f) * invSize, (static_cast<float>(y) + 0.5f) * invSize);
				const float w0 = edgeFunction(uv[1], uv[2], p) * invArea;
				const float w1 = edgeFunction(uv[2], uv[0], p) * invArea;
				const float w2 = 1.0f - w0 - w1;
				// A small negative tolerance closes the hairline cracks between triangles
				// that share an edge; the UV islands gain a texel of padding, which the
				// dilation pass would add anyway.
				constexpr float kTolerance = -1e-3f;
				if (w0 < kTolerance || w1 < kTolerance || w2 < kTolerance) continue;

				SurfaceTexel& texel = surface[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)];
				const glm::vec3 position = tri->p[0] * w0 + tri->p[1] * w1 + tri->p[2] * w2;
				const glm::vec3 normal = tri->n[0] * w0 + tri->n[1] * w1 + tri->n[2] * w2;
				if (texel.weight == 0.0f) ++coveredTexels;
				texel.position += position;
				texel.normal += normal;
				texel.weight += 1.0f;
			}
		}
	}
}

/// @brief Averages covered texels, adds padding rings into empty ones, then blurs.
void finishMap(std::vector<float>& ao, std::vector<std::uint8_t>& valid, std::uint32_t size,
	int dilate, int blur) {
	const auto at = [size](std::uint32_t x, std::uint32_t y) { return static_cast<std::size_t>(y) * size + x; };

	// Dilation: copy known values into empty texels, one ring per pass. Needed because a
	// sampler filtering at an island's edge would otherwise read the cleared 0.
	for (int pass = 0; pass < dilate; ++pass) {
		std::vector<std::uint8_t> added(valid.size(), 0);
		bool any = false;
		for (std::uint32_t y = 0; y < size; ++y) {
			for (std::uint32_t x = 0; x < size; ++x) {
				const std::size_t index = at(x, y);
				if (valid[index]) continue;
				float sum = 0.0f;
				float count = 0.0f;
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						const int nx = static_cast<int>(x) + dx;
						const int ny = static_cast<int>(y) + dy;
						if (nx < 0 || ny < 0 || nx >= static_cast<int>(size) || ny >= static_cast<int>(size)) continue;
						const std::size_t n = at(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
						if (!valid[n]) continue;
						sum += ao[n];
						count += 1.0f;
					}
				}
				if (count > 0.0f) { ao[index] = sum / count; added[index] = 1; any = true; }
			}
		}
		if (!any) break;
		for (std::size_t i = 0; i < valid.size(); ++i) valid[i] |= added[i];
	}

	// Box blur (also removes the leftover sampling noise).
	if (blur > 0) {
		std::vector<float> temp(ao.size(), 0.0f);
		const int radius = blur;
		for (std::uint32_t y = 0; y < size; ++y) {
			for (std::uint32_t x = 0; x < size; ++x) {
				float sum = 0.0f; float count = 0.0f;
				for (int dx = -radius; dx <= radius; ++dx) {
					const int nx = static_cast<int>(x) + dx;
					if (nx < 0 || nx >= static_cast<int>(size)) continue;
					const std::size_t n = at(static_cast<std::uint32_t>(nx), y);
					if (!valid[n]) continue;
					sum += ao[n]; count += 1.0f;
				}
				temp[at(x, y)] = count > 0.0f ? sum / count : ao[at(x, y)];
			}
		}
		for (std::uint32_t y = 0; y < size; ++y) {
			for (std::uint32_t x = 0; x < size; ++x) {
				float sum = 0.0f; float count = 0.0f;
				for (int dy = -radius; dy <= radius; ++dy) {
					const int ny = static_cast<int>(y) + dy;
					if (ny < 0 || ny >= static_cast<int>(size)) continue;
					const std::size_t n = at(x, static_cast<std::uint32_t>(ny));
					if (!valid[n]) continue;
					sum += temp[n]; count += 1.0f;
				}
				if (count > 0.0f) ao[at(x, y)] = sum / count;
			}
		}
	}
}

} // namespace

bool bakeAll(const ModelGeometry& model, const BakeSettings& settings,
	std::vector<AoMap>& out, std::string& error) {
	if (model.tris.empty()) { error = "no geometry"; return false; }
	if (settings.size == 0 || settings.size > 8192) { error = "size must be between 1 and 8192"; return false; }
	if (settings.samples == 0) { error = "samples must be at least 1"; return false; }

	const auto startTime = std::chrono::steady_clock::now();
	const Bvh bvh = buildModelBvh(model);
	const glm::vec3 extent = model.boundsMax - model.boundsMin;
	const float diagonal = glm::length(extent);
	if (!(diagonal > 0.0f)) { error = "the model has zero extent"; return false; }

	const float bias = settings.bias > 0.0f ? settings.bias : diagonal * 1e-4f;
	const float maxT = settings.distance > 0.0f ? settings.distance : std::numeric_limits<float>::max();
	const std::uint32_t size = settings.size;
	int threadCount = settings.threads;
	if (threadCount <= 0) threadCount = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
	threadCount = std::min(threadCount, 64);

	// Group the UV-mapped triangles per material.
	std::vector<std::vector<const Tri*>> perMaterial(model.materialCount);
	std::size_t skippedNoUv = 0;
	for (const Tri& tri : model.tris) {
		if (tri.material == UINT32_MAX || tri.material >= model.materialCount) { ++skippedNoUv; continue; }
		if (!tri.hasUv) { ++skippedNoUv; continue; }
		perMaterial[tri.material].push_back(&tri);
	}

	out.clear();
	for (std::uint32_t material = 0; material < model.materialCount; ++material) {
		const std::vector<const Tri*>& tris = perMaterial[material];
		if (tris.empty()) continue;
		const auto materialStart = std::chrono::steady_clock::now();

		std::vector<float> ao(static_cast<std::size_t>(size) * size, 0.0f);
		std::vector<std::uint8_t> valid(static_cast<std::size_t>(size) * size, 0);
		std::vector<SurfaceTexel> surface(static_cast<std::size_t>(size) * size);
		std::size_t covered = 0;
		rasterizeUv(tris, size, settings.flipV, surface, covered);
		if (settings.verbose) {
			std::printf("  material %-3u: %zu UV triangle(s), %zu covered texel(s)\n", material, tris.size(), covered);
		}
		if (covered == 0) {
			if (settings.verbose) std::printf("  material %u: no UV coverage, skipped\n", material);
			continue;
		}

		// Per-texel AO, rows distributed across the workers. Every texel is independent, so
		// no locking is needed beyond the work counter.
		std::atomic<std::uint32_t> nextRow{ 0 };
		std::vector<std::thread> workers;
		workers.reserve(static_cast<std::size_t>(threadCount));
		for (int worker = 0; worker < threadCount; ++worker) {
			workers.emplace_back([&] {
				for (;;) {
					const std::uint32_t y = nextRow.fetch_add(1, std::memory_order_relaxed);
					if (y >= size) return;
					for (std::uint32_t x = 0; x < size; ++x) {
						const std::size_t index = static_cast<std::size_t>(y) * size + x;
						SurfaceTexel& texel = surface[index];
						if (texel.weight <= 0.0f) continue;
						const glm::vec3 position = texel.position / texel.weight;
						glm::vec3 normal = texel.normal;
						const float len2 = glm::dot(normal, normal);
						if (len2 < 1e-12f) continue; // opposing normals cancelled out
						normal /= std::sqrt(len2);
						valid[index] = 1;

						glm::vec3 tangent, bitangent;
						buildOnb(normal, tangent, bitangent);
						const glm::vec3 origin = position + normal * bias;

						// Per-texel rotation of the Hammersley sequence, so neighbouring
						// texels do not share one visible sample pattern.
						const std::uint32_t seed = hash32(x * 1973u + y * 9277u + 26699u);
						const float rotU = static_cast<float>(seed & 0xFFFFu) / 65536.0f;
						const float rotV = static_cast<float>((seed >> 16) & 0xFFFFu) / 65536.0f;

						std::uint32_t occludedCount = 0;
						for (std::uint32_t s = 0; s < settings.samples; ++s) {
							const float u1 = std::fmod((static_cast<float>(s) + 0.5f) / settings.samples + rotU, 1.0f);
							const float u2 = std::fmod(radicalInverse(s) + rotV, 1.0f);
							const glm::vec3 dir = sampleCosineHemisphere(tangent, bitangent, normal, u1, u2);
							if (bvh.occluded(origin, dir, maxT)) ++occludedCount;
						}
						ao[index] = 1.0f - static_cast<float>(occludedCount) / static_cast<float>(settings.samples);
					}
				}
			});
		}
		for (auto& worker : workers) worker.join();

		finishMap(ao, valid, size, settings.dilate, settings.blur);

		// Contrast and 8-bit quantization; stats so the run can be checked without opening
		// the image.
		float minimum = 1.0f, maximum = 0.0f, sum = 0.0f;
		std::size_t count = 0;
		for (std::size_t i = 0; i < ao.size(); ++i) {
			if (!valid[i]) continue;
			float value = std::clamp(ao[i], 0.0f, 1.0f);
			if (settings.power != 1.0f) value = std::pow(value, settings.power);
			ao[i] = value;
			minimum = std::min(minimum, value);
			maximum = std::max(maximum, value);
			sum += value;
			++count;
		}

		AoMap map;
		map.material = material;
		map.name = (material < model.materialNames.size() && !model.materialNames[material].empty())
			? model.materialNames[material] + "_AO"
			: "AO" + std::to_string(material);
		map.width = size;
		map.height = size;
		map.rgb.resize(static_cast<std::size_t>(size) * size * 3, 255);
		for (std::size_t i = 0; i < ao.size(); ++i) {
			const std::uint8_t value = valid[i] ? static_cast<std::uint8_t>(std::lround(std::clamp(ao[i], 0.0f, 1.0f) * 255.0f)) : 255;
			map.rgb[i * 3 + 0] = value;
			map.rgb[i * 3 + 1] = value;
			map.rgb[i * 3 + 2] = value;
		}
		out.push_back(std::move(map));

		if (settings.verbose) {
			const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - materialStart).count();
			const float coveredFraction = 100.0f * static_cast<float>(covered) / static_cast<float>(size) / static_cast<float>(size);
			std::printf("  material %-3u (%s): %.1f%% covered, AO min %.3f mean %.3f max %.3f, %.2fs\n",
				material, out.back().name.c_str(), coveredFraction,
				minimum, count ? sum / static_cast<float>(count) : 1.0f, maximum, elapsed);
		}
	}

	if (out.empty()) {
		error = "no material had UV-mapped triangles to bake";
		return false;
	}
	if (settings.verbose) {
		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
		std::printf("  baked %zu map(s) at %ux%u, %u samples/texel, %d threads, %.2fs"
			" (skipped %zu triangle(s) without a material or UVs)\n",
			out.size(), size, size, settings.samples, threadCount, elapsed, skippedNoUv);
	}
	return true;
}

} // namespace aob
