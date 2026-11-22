#pragma once
/**
 * @file TangentSpace.hpp
 * @brief Per-vertex tangent generation for normal mapping.
 */
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

/**
 * @brief Generates one tangent per vertex from the mesh's UV layout (Lengyel's method).
 *
 * The normal map's tangent space is defined by the asset, not by the screen: without a
 * per-vertex tangent the shader can only rebuild a frame from screen-space derivatives, and
 * the sign that maps the asset's convention into that frame becomes a per-asset guess (the
 * "NM Flip X/Y" knobs). A real tangent removes the guess: `tangent.xyz` is the surface's +u
 * direction and `tangent.w` is the handedness (+1 when `cross(normal, tangent)` points along
 * +v, -1 when the UVs are mirrored), which is exactly what glTF's `TANGENT` attribute and
 * MikkTSpace store.
 *
 * Per triangle the UV gradient gives dP/du and dP/dv; those are accumulated on the three
 * vertices (weighted by nothing, as in Lengyel's reference), then per vertex the accumulated
 * tangent is orthogonalized against the normal (Gram-Schmidt) and normalized, and its sign
 * against the accumulated bitangent becomes `w`. MikkTSpace differs only in how it handles
 * UV seams and degenerate triangles; for assets that ship no tangents this is the standard
 * approximation, and an asset that does ship them should keep them (see the loader).
 *
 * @param positions Per-vertex positions.
 * @param normals   Per-vertex normals (must be normalized; a zero normal falls back to +Y).
 * @param uvs       Per-vertex texture coordinates.
 * @param indices   Triangle list into the vertex arrays; empty means "sequential".
 * @param outTangents Receives one Vec4 per vertex, `xyz` = tangent, `w` = handedness.
 */
EE_API void generateTangents(const Vector<Vec3>& positions, const Vector<Vec3>& normals, const Vector<Vec2>& uvs,
	const Vector<UInt32>& indices, Vector<Vec4>& outTangents);

EE_NAMESPACE_RENDERING_END
