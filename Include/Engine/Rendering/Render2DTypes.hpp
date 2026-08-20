#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_RENDERING_BEGIN

/**
 * @brief 2D vertex with position, UV, and color.
 */
struct Vertex2D {
	Vec2 pos;     ///< Screen-space position.
	Vec2 uv;      ///< Texture coordinates.
	Vec4 color;   ///< RGBA vertex color.
};

/**
 * @brief 2D sprite draw descriptor.
 */
struct SpriteDesc {
	Vec2  position = Vec2(0);
	Vec2  size     = Vec2(100);
	Vec4  color    = Vec4(1);
	float rotation = 0;
	void* texture  = nullptr;  ///< Optional texture SRV (ITextureView*).
	Vec2  uvMin    = Vec2(0);  ///< Optional custom UV (top-left).
	Vec2  uvMax    = Vec2(1);  ///< Optional custom UV (bottom-right).
};

EE_NAMESPACE_RENDERING_END
