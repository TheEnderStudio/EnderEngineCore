#include <Rendering/TangentSpace.hpp>

#include <cmath>

EE_NAMESPACE_RENDERING_BEGIN

void generateTangents(const Vector<Vec3>& positions, const Vector<Vec3>& normals, const Vector<Vec2>& uvs,
	const Vector<UInt32>& indices, Vector<Vec4>& outTangents) {
	const size_t vertexCount = positions.size();
	outTangents.assign(vertexCount, Vec4(1.0f, 0.0f, 0.0f, 1.0f));
	if (vertexCount == 0 || uvs.size() < vertexCount) return;

	Vector<Vec3> tanAcc(vertexCount, Vec3(0.0f));
	Vector<Vec3> bitAcc(vertexCount, Vec3(0.0f));

	const size_t triangleCount = indices.empty() ? vertexCount / 3 : indices.size() / 3;
	for (size_t t = 0; t < triangleCount; ++t) {
		const UInt32 i0 = indices.empty() ? static_cast<UInt32>(t * 3 + 0) : indices[t * 3 + 0];
		const UInt32 i1 = indices.empty() ? static_cast<UInt32>(t * 3 + 1) : indices[t * 3 + 1];
		const UInt32 i2 = indices.empty() ? static_cast<UInt32>(t * 3 + 2) : indices[t * 3 + 2];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;

		const Vec3 e1 = positions[i1] - positions[i0];
		const Vec3 e2 = positions[i2] - positions[i0];
		const Vec2 d1 = uvs[i1] - uvs[i0];
		const Vec2 d2 = uvs[i2] - uvs[i0];

		// Solve [e1 e2] = [T B] * [d1 d2]; a degenerate (or mirrored to zero) UV triangle
		// carries no tangent information at all.
		const float det = d1.x * d2.y - d2.x * d1.y;
		if (std::fabs(det) < 1e-12f) continue;
		const float invDet = 1.0f / det;
		const Vec3 tangent = (e1 * d2.y - e2 * d1.y) * invDet;
		const Vec3 bitangent = (e2 * d1.x - e1 * d2.x) * invDet;

		const UInt32 v[3] = { i0, i1, i2 };
		for (int k = 0; k < 3; ++k) {
			tanAcc[v[k]] += tangent;
			bitAcc[v[k]] += bitangent;
		}
	}

	for (size_t i = 0; i < vertexCount; ++i) {
		Vec3 n = normals.size() > i ? normals[i] : Vec3(0.0f, 1.0f, 0.0f);
		const float nLen2 = glm::dot(n, n);
		n = nLen2 > 1e-12f ? n / std::sqrt(nLen2) : Vec3(0.0f, 1.0f, 0.0f);

		Vec3 t = tanAcc[i] - n * glm::dot(n, tanAcc[i]); // Gram-Schmidt
		float tLen2 = glm::dot(t, t);
		if (tLen2 < 1e-12f) {
			// No usable UV gradient at this vertex (a seam vertex shared by degenerate
			// triangles, or a mesh without UVs): pick any direction perpendicular to the
			// normal instead of returning a zero tangent, which would make the TBN singular.
			const Vec3 up = std::fabs(n.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
			t = glm::cross(up, n);
			tLen2 = glm::dot(t, t);
			if (tLen2 < 1e-12f) t = Vec3(1.0f, 0.0f, 0.0f);
			else t /= std::sqrt(tLen2);
			outTangents[i] = Vec4(t, 1.0f);
			continue;
		}
		t /= std::sqrt(tLen2);

		// Handedness: +1 when the accumulated bitangent agrees with cross(n, t), which is
		// what a non-mirrored UV layout produces; -1 for mirrored UVs, where the bitangent
		// has to be negated (glTF's TANGENT.w, MikkTSpace's `fMagS` sign).
		const float w = glm::dot(glm::cross(n, t), bitAcc[i]) < 0.0f ? -1.0f : 1.0f;
		outTangents[i] = Vec4(t, w);
	}
}

EE_NAMESPACE_RENDERING_END
