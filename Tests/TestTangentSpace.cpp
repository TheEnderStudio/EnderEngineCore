#include <gtest/gtest.h>

#include <Rendering/TangentSpace.hpp>

#include <cmath>

using namespace EnderEngine;
using namespace EnderEngine::Rendering;

namespace {

/// A quad in the XZ plane, normal +Y, two triangles.
void makeQuad(const Vec2& uv0, const Vec2& uv1, const Vec2& uv2, const Vec2& uv3,
	Vector<Vec3>& pos, Vector<Vec3>& nrm, Vector<Vec2>& uv, Vector<UInt32>& idx) {
	pos = { Vec3(-1, 0, -1), Vec3(1, 0, -1), Vec3(1, 0, 1), Vec3(-1, 0, 1) };
	nrm = { Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0) };
	uv = { uv0, uv1, uv2, uv3 };
	idx = { 0, 1, 2, 0, 2, 3 };
}

} // namespace

TEST(TangentSpace, TangentFollowsTheUvDirection)
{
	Vector<Vec3> pos, nrm; Vector<Vec2> uv; Vector<UInt32> idx;
	// u grows along +x, v grows along +z: the tangent must be +x.
	makeQuad(Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1), pos, nrm, uv, idx);
	Vector<Vec4> tan;
	generateTangents(pos, nrm, uv, idx, tan);
	ASSERT_EQ(tan.size(), pos.size());
	for (const Vec4& t : tan) {
		EXPECT_NEAR(t.x, 1.0f, 1e-4f);
		EXPECT_NEAR(t.y, 0.0f, 1e-4f);
		EXPECT_NEAR(t.z, 0.0f, 1e-4f);
		// glTF/MikkTSpace convention: bitangent = cross(normal, tangent.xyz) * w. With
		// n = +y, t = +x, cross(n, t) is -z, while dP/dv here is +z, so the stored
		// handedness is -1. A mirrored v flips it to +1.
		EXPECT_NEAR(t.w, -1.0f, 1e-4f);
	}
}

TEST(TangentSpace, MirroredUvsFlipTheHandedness)
{
	Vector<Vec3> pos, nrm; Vector<Vec2> uv; Vector<UInt32> idx;
	// u still grows along +x, but the v direction is mirrored: the bitangent has to point
	// the other way, which is exactly what the stored handedness encodes.
	makeQuad(Vec2(0, 0), Vec2(1, 0), Vec2(1, -1), Vec2(0, -1), pos, nrm, uv, idx);
	Vector<Vec4> tan;
	generateTangents(pos, nrm, uv, idx, tan);
	float w = 0.0f;
	for (const Vec4& t : tan) {
		EXPECT_NEAR(t.x, 1.0f, 1e-4f);
		w = t.w;
		EXPECT_GT(t.w, 0.0f); // mirrored relative to the non-mirrored case, which stores -1
	}
	// The bitangent recovered from the frame must agree with dP/dv, which points along -z.
	const Vec3 n(0, 1, 0);
	const Vec3 t(tan[0].x, tan[0].y, tan[0].z);
	const Vec3 b = glm::cross(n, t) * w;
	EXPECT_LT(b.z, 0.0f);
}

TEST(TangentSpace, TangentIsOrthogonalToTheNormalAndUnitLength)
{
	Vector<Vec3> pos, nrm; Vector<Vec2> uv; Vector<UInt32> idx;
	// A slanted quad so the accumulated tangent is not perpendicular to the normal to begin
	// with: the Gram-Schmidt step has real work to do.
	makeQuad(Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1), pos, nrm, uv, idx);
	for (Vec3& p : pos) p.y = p.z * 0.5f;
	nrm = { Vec3(0, 1, -0.5f), Vec3(0, 1, -0.5f), Vec3(0, 1, -0.5f), Vec3(0, 1, -0.5f) };
	for (Vec3& n : nrm) n = glm::normalize(n);
	Vector<Vec4> tan;
	generateTangents(pos, nrm, uv, idx, tan);
	for (size_t i = 0; i < tan.size(); ++i) {
		const Vec3 t(tan[i].x, tan[i].y, tan[i].z);
		EXPECT_NEAR(glm::dot(t, nrm[i]), 0.0f, 1e-4f);
		EXPECT_NEAR(glm::length(t), 1.0f, 1e-4f);
	}
}

TEST(TangentSpace, MissingUvsStillProduceAUsableFrame)
{
	Vector<Vec3> pos{ Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0) };
	Vector<Vec3> nrm{ Vec3(0, 0, 1), Vec3(0, 0, 1), Vec3(0, 0, 1) };
	Vector<Vec2> uv; // none
	Vector<UInt32> idx{ 0, 1, 2 };
	Vector<Vec4> tan;
	generateTangents(pos, nrm, uv, idx, tan);
	ASSERT_EQ(tan.size(), 3u);
	for (size_t i = 0; i < tan.size(); ++i) {
		const Vec3 t(tan[i].x, tan[i].y, tan[i].z);
		EXPECT_FALSE(std::isnan(t.x) || std::isnan(t.y) || std::isnan(t.z));
		EXPECT_NEAR(glm::length(t), 1.0f, 1e-4f);
		EXPECT_NEAR(glm::dot(t, nrm[i]), 0.0f, 1e-4f); // still perpendicular to the normal
	}
}

TEST(TangentSpace, EmptyMeshIsHandled)
{
	Vector<Vec3> pos, nrm; Vector<Vec2> uv; Vector<UInt32> idx; Vector<Vec4> tan;
	generateTangents(pos, nrm, uv, idx, tan);
	EXPECT_TRUE(tan.empty());
}
