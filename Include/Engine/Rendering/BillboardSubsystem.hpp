#pragma once

#include <Engine/Core/Subsystem.hpp>
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

/// @brief Descriptor for a billboard (camera-facing quad with optional rotation).
struct BillboardDesc {
	Vec3  position;
	Vec2  size;
	Vec4  color;
	F32   rotation = 0.0f;
};

/// @brief Subsystem for rendering camera-facing billboard quads.
class EE_API BillboardSubsystem : public Subsystem {
public:
	/// @brief Default constructor.
	BillboardSubsystem();
	/// @brief Destructor.
	~BillboardSubsystem() override;

	EE_NO_COPY(BillboardSubsystem)
	EE_NO_MOVE(BillboardSubsystem)

	/// @brief Attach to a RenderSubsystem for access to device/context.
	void attachToRenderer(class RenderSubsystem* renderer);

	/// @brief Draw billboards as camera-facing quads with alpha blending.
	void draw(const Vector<BillboardDesc>& billboards);

	/// @brief Shortcut: draw a single fog-like cloud.
	void drawFogCloud(const Vec3& center, F32 radius, F32 opacity = 0.4f);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
