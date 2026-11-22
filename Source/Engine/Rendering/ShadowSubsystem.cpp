#include <Rendering/ShadowSubsystem.hpp>
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>
#include <array>

#include <DiligentFX/Components/interface/ShadowMapManager.hpp>
#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

struct ShadowSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	ShadowConfig cfg;
	D::ShadowMapManager mgr;
	D::RefCntAutoPtr<D::ISampler> cmpSampler;
	D::RefCntAutoPtr<D::IBuffer> frameCB;
	Diligent::ShadowMapAttribs shadowAttribs;
	/// Cascade transforms as handed to the shader: the manager's, with the optional
	/// light-space depth extension already applied (see distribute()).
	Mat4 cascadeXf[4] = {};
	bool initialized = false;
	bool loggedSplits = false; ///< One-shot: print the cascade split distances (unit check).
};

ShadowSubsystem::ShadowSubsystem() : Subsystem("Shadow"), m_impl(std::make_unique<Impl>()) {}
ShadowSubsystem::~ShadowSubsystem() = default;

void ShadowSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }
void ShadowSubsystem::setConfig(const ShadowConfig& cfg) { m_impl->cfg = cfg; m_impl->initialized = false; }
// The extension is read every frame in distribute(), so it can be changed live.
void ShadowSubsystem::setCasterExtension(F32 worldUnits) { m_impl->cfg.casterExtension = worldUnits < 0.0f ? 0.0f : worldUnits; }
F32 ShadowSubsystem::casterExtension() const { return m_impl->cfg.casterExtension; }

TextureSRV ShadowSubsystem::getSRV() const { return m_impl->mgr.GetSRV(); }

TextureDSV ShadowSubsystem::getCascadeDSV(UInt32 c) const { return m_impl->mgr.GetCascadeDSV(c); }

Mat4 ShadowSubsystem::getCascadeTransform(UInt32 c) const {
	return m_impl->cascadeXf[c < 4 ? c : 0];
}

Mat4 ShadowSubsystem::getWorldToShadowMapUVDepth(UInt32 ci) const {
	Mat4 wlp = m_impl->cascadeXf[ci < 4 ? ci : 0];
	Mat4 p2uv = Mat4(1.0f);
	p2uv[0][0] = 0.5f; p2uv[1][1] = -0.5f; p2uv[2][2] = 1.0f;
	p2uv[3][0] = 0.5f; p2uv[3][1] = 0.5f;
	return p2uv * wlp;
}

Vec4 ShadowSubsystem::getCascadeSplitDistances() const {
	auto& a = m_impl->shadowAttribs;
	return Vec4(a.fCascadeCamSpaceZEnd[0], a.fCascadeCamSpaceZEnd[1], a.fCascadeCamSpaceZEnd[2], a.fCascadeCamSpaceZEnd[3]);
}

const ShadowConfig& ShadowSubsystem::config() const { return m_impl->cfg; }

Result<void, CoreError> ShadowSubsystem::onInitialize() {
	auto& p = *m_impl;
	if (!p.renderer) { EError("Shadow: no renderer attached"); return CoreError::OperationFailed; }
	if (!p.cfg.enabled) { EInfo("Shadow: disabled"); return {}; }

	auto* device = static_cast<D::IRenderDevice*>(p.renderer->getDevice());
	auto* ctx    = static_cast<D::IDeviceContext*>(p.renderer->getContext());

	// Comparison sampler
	D::SamplerDesc sd;
	sd.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.ComparisonFunc = D::COMPARISON_FUNC_LESS;
	sd.AddressU = D::TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D::TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D::TEXTURE_ADDRESS_CLAMP;
	device->CreateSampler(sd, &p.cmpSampler);

	// Frame CB for shadow pass
	D::BufferDesc bd; bd.Name = "ShadowFrameCB";
	bd.Size = 256; bd.BindFlags = D::BIND_UNIFORM_BUFFER;
	bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
	device->CreateBuffer(bd, nullptr, &p.frameCB);

	// Init shadow manager
	D::ShadowMapManager::InitInfo si;
	si.Format      = D::TEX_FORMAT_D16_UNORM;
	si.Resolution  = p.cfg.resolution;
	si.NumCascades = p.cfg.numCascades;
	si.ShadowMode  = 1;
	si.pComparisonSampler = p.cmpSampler;
	si.Is32BitFilterableFmt = false;
	p.mgr.Initialize(device, nullptr, si);
	p.initialized = true;

	EInfo("Shadow: {} cascades @ {}x{} ready", p.cfg.numCascades, p.cfg.resolution, p.cfg.resolution);
	return {};
}

void ShadowSubsystem::onShutdown() {
	m_impl->mgr = D::ShadowMapManager{};
	m_impl->initialized = false;
}

void ShadowSubsystem::distribute(const Vec3& lightDir, const Vec3& eye, const Vec3& center, const Vec3& up, F32 fov, F32 aspect, F32 nearP, F32 farP) {
	auto& p = *m_impl;
	if (!p.initialized) return;

	Mat4 viewLH = glm::lookAtLH(eye, center, up);
	// Build D3D LH perspective manually (_34=1, _43=-zn*zf/(zf-zn))
	Mat4 projLH(0);
	F32 f = 1.0f / tanf(fov * 0.5f);
	projLH[0][0] = f / aspect;
	projLH[1][1] = f;
	projLH[2][2] = farP / (farP - nearP);
	projLH[2][3] = -(farP * nearP) / (farP - nearP);
	projLH[3][2] = 1.0f;

	D::float4x4 view, proj;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++) {
			view[r][c] = viewLH[c][r];
			proj[r][c] = projLH[c][r];
		}

	D::ShadowMapManager::DistributeCascadeInfo di;
	di.pCameraView  = &view;
	di.pCameraProj  = &proj;
	D::float3 d = { lightDir.x, lightDir.y, lightDir.z };
	di.pLightDir    = &d;
	di.fPartitioningFactor = p.cfg.partitioning;
	di.SnapCascades = true;
	di.StabilizeExtents = true;
	di.EqualizeExtents = true;
	p.mgr.DistributeCascades(di, p.shadowAttribs);

	// Copy the cascades out, optionally growing their ortho boxes.
	//
	// DistributeCascades fits each cascade's box to the camera slice alone, and a caster
	// does not have to sit inside that slice: geometry between the light and the receiver,
	// or off to the side of it, projects outside the box and is clipped away. The shadow
	// then disappears exactly when the camera gets close, because the near cascades have
	// the tightest boxes - and widening only the depth range does nothing for a caster that
	// is outside in x/y, which is why a z-only widening left the bug untouched.
	//
	// Growing the box uniformly is a remap of the matrix rows 0..2: NDC x/y/z are linear in
	// world position for an orthographic light projection, so scaling a row about the NDC
	// centre 0.5 enlarges that axis symmetrically: row' = row / s with the constant term
	// ((row.w - 0.5) / s + 0.5). The world size of the box along a row is 1 / |row.xyz|, so
	// asking for extension world units on every side gives s = 1 + 2 * extension / box.
	// Rendering and sampling use these copies, so the two can never disagree.
	for (UInt32 c = 0; c < p.cfg.numCascades && c < 4; ++c) {
		const auto& src = p.mgr.GetCascadeTranform(c).WorldToLightProjSpace;
		Mat4 m = *reinterpret_cast<const Mat4*>(&src);
		const F32 extension = p.cfg.casterExtension;
		if (extension > 0.0f) {
			// glm is column major: element (row, col) is m[col][row].
			const Vec4 rowX(m[0][0], m[1][0], m[2][0], m[3][0]);
			const Vec4 rowY(m[0][1], m[1][1], m[2][1], m[3][1]);
			const F32 gradient = std::max(std::sqrt(rowX.x * rowX.x + rowX.y * rowX.y + rowX.z * rowX.z),
				std::sqrt(rowY.x * rowY.x + rowY.y * rowY.y + rowY.z * rowY.z));
			if (gradient > 1e-8f) {
				const F32 boxWorld = 1.0f / gradient;          // world size covered by the box
				const F32 scale = 1.0f + 2.0f * extension / std::max(boxWorld, 1e-6f);
				const F32 k = 1.0f / scale;
				for (int r = 0; r < 3; ++r) {
					const F32 x = m[0][r], y = m[1][r], z = m[2][r], w = m[3][r];
					m[0][r] = x * k;
					m[1][r] = y * k;
					m[2][r] = z * k;
					m[3][r] = (w - 0.5f) * k + 0.5f;
				}
			}
		}
		p.cascadeXf[c] = m;
	}	for (UInt32 c = p.cfg.numCascades; c < 4; ++c) {
		const UInt32 last = p.cfg.numCascades > 0 ? p.cfg.numCascades - 1 : 0;
		p.cascadeXf[c] = p.cascadeXf[last];
	}
	if (!p.loggedSplits) {
		p.loggedSplits = true;
		const auto& a = p.shadowAttribs;
		EInfo("Shadow: cascades end at camera Z {:.2f} / {:.2f} / {:.2f} / {:.2f} (caster extension {:.1f})",
			a.fCascadeCamSpaceZEnd[0], a.fCascadeCamSpaceZEnd[1], a.fCascadeCamSpaceZEnd[2], a.fCascadeCamSpaceZEnd[3],
			p.cfg.casterExtension);
	}
}

EE_NAMESPACE_RENDERING_END
