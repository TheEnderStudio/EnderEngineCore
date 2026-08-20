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
	bool initialized = false;
};

ShadowSubsystem::ShadowSubsystem() : Subsystem("Shadow"), m_impl(std::make_unique<Impl>()) {}
ShadowSubsystem::~ShadowSubsystem() = default;

void ShadowSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }
void ShadowSubsystem::setConfig(const ShadowConfig& cfg) { m_impl->cfg = cfg; m_impl->initialized = false; }

TextureSRV ShadowSubsystem::getSRV() const { return m_impl->mgr.GetSRV(); }

TextureDSV ShadowSubsystem::getCascadeDSV(UInt32 c) const { return m_impl->mgr.GetCascadeDSV(c); }

Mat4 ShadowSubsystem::getCascadeTransform(UInt32 c) const {
	const auto& xf = m_impl->mgr.GetCascadeTransform(c).WorldToLightProjSpace;
	return *reinterpret_cast<const Mat4*>(&xf);
}

Mat4 ShadowSubsystem::getWorldToShadowMapUVDepth(UInt32 ci) const {
	const auto& xf = m_impl->mgr.GetCascadeTransform(ci).WorldToLightProjSpace;
	Mat4 wlp = *reinterpret_cast<const Mat4*>(&xf);
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
}

EE_NAMESPACE_RENDERING_END
