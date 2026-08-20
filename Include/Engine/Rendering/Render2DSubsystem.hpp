#pragma once

#include <Core/Subsystem.hpp>
#include "Render2DTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

/**
 * @brief 2D rendering subsystem (sprite batch).
 *
 * Renders quads in screen space with optional textures and colors.
 * Uses batched draw calls — all sprites submitted between begin()/end()
 * are drawn in one GPU call per texture.
 */
class EE_API Render2DSubsystem : public Subsystem {
public:
	Render2DSubsystem();
	~Render2DSubsystem() override;

	EE_NO_COPY(Render2DSubsystem)
	EE_NO_MOVE(Render2DSubsystem)

	void setDevice(void* device);
	void setContext(void* context);

	/**
	 * @brief Set the screen dimensions for orthographic projection.
	 */
	void setScreenSize(UInt32 w, UInt32 h);

	/**
	 * @brief Set whether 2D is rendered after post-processing.
	 *
	 * When true (default), the 2D PSO expects no depth buffer (post-process
	 * already unbound it). When false, expects the scene depth buffer (D32).
	 * @param after true = after post-process, false = before.
	 */
	void setAfterPostProcess(bool after);

	/**
	 * @brief Begin a 2D rendering batch. Resets the vertex buffer.
	 */
	void begin();

	/**
	 * @brief Submit a sprite to the batch.
	 * @param desc Sprite description.
	 */
	void drawSprite(const SpriteDesc& desc);

	/**
	 * @brief Submit a sprite with custom UV coordinates.
	 */
	void drawSpriteUV(const SpriteDesc& desc);

	/**
	 * @brief Submit a colored rectangle.
	 * @param pos   Top-left position.
	 * @param size  Width and height.
	 * @param color RGBA fill color.
	 */
	void drawRect(Vec2 pos, Vec2 size, Vec4 color);

	/**
	 * @brief Draw a filled circle approximated by triangles.
	 * @param center Center position.
	 * @param radius Radius in pixels.
	 * @param color  Fill color.
	 * @param segments Number of segments (default 32).
	 */
	void drawFilledCircle(Vec2 center, F32 radius, Vec4 color, UInt32 segments = 32);

	/**
	 * @brief Draw an outlined circle (ring).
	 * @param center Center position.
	 * @param radius Outer radius in pixels.
	 * @param thickness Line thickness in pixels.
	 * @param color  Color.
	 * @param segments Number of segments (default 32).
	 */
	void drawOutlineCircle(Vec2 center, F32 radius, F32 thickness, Vec4 color, UInt32 segments = 32);

	/// @brief Set scissor rect for clipping. Pass (0,0) size to disable.
	void setScissorRect(Vec2 pos, Vec2 size);

	/**
	 * @brief Flush the batch and render all submitted sprites.
	 */
	void end();

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
