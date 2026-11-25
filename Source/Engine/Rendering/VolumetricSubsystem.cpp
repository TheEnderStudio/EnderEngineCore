#include <Rendering/VolumetricSubsystem.hpp>
#include "VolumetricBackend.hpp"
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>

#include <algorithm>
#include <cmath>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

struct VolumetricSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	VolumetricConfig cfg;
	/// The active algorithm. Rebuilt when the configured mode changes.
	Uptr<Detail::IVolumetricBackend> backend;
	VolumetricConfig::Mode builtMode = VolumetricConfig::Mode::ScreenSpace;
	bool built = false;
	bool ready = false;

	/// Shared temporal accumulation, applied to whichever backend produced the frame.
	Uptr<Detail::ITemporalResolve> temporal;
	/// Set when the history must not be reused (backend rebuilt, or the temporal switch
	/// toggled off and on).
	bool resetHistory = true;
	/// The SRV the composite reads: the resolved history when accumulation runs, otherwise
	/// the backend's own output.
	void* resolvedSRV = nullptr;

	/// Tileable 3D value noise for the medium's density modulation, generated once.
	D::RefCntAutoPtr<D::ITexture> noiseTex;
	D::RefCntAutoPtr<D::ITextureView> noiseSRV;

	/// Adaptive-quality scale applied to the configured steps/slices (1 = as configured).
	F32 qualityScale = 1.0f;
	/// Rolling mean of the backend's pass time, for the readout and the governor.
	F32 averageMs = 0.0f;
	bool averaged = false;

	/// @brief Creates the backend for a mode, shutting the previous one down first.
	bool buildBackend(VolumetricConfig::Mode mode) {
		if (backend) {
			backend->shutdown();
			backend.reset();
		}
		backend = (mode == VolumetricConfig::Mode::Froxel) ? Detail::createFroxelBackend()
			: Detail::createScreenSpaceBackend();
		builtMode = mode;
		built = true;
		resetHistory = true;
		if (!backend) return false;
		if (!backend->initialize(renderer)) {
			EError("Volumetric: '{}' backend failed to initialize; fog disabled", backend->name());
			backend.reset();
			return false;
		}
		EInfo("Volumetric: '{}' backend active", backend->name());
		return true;
	}
};

namespace {

/// @brief Generates a tileable 3D value-noise volume (R8, trilinear, wrapping).
///
/// 64^3 with three octaves is enough for fog wisps: the medium is low-frequency by nature, and
/// the texture is sampled with a wrapping address mode so it tiles in every direction.
bool createNoiseVolume(D::IRenderDevice* device, D::RefCntAutoPtr<D::ITexture>& tex,
	D::RefCntAutoPtr<D::ITextureView>& srv) {
	constexpr UInt32 kSize = 64;
	if (!device) return false;

	// Deterministic hash-based lattice, so the volume is identical on every run.
	auto hash = [](UInt32 x, UInt32 y, UInt32 z) {
		UInt32 h = x * 374761393u + y * 668265263u + z * 2147483647u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return (F32)((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
	};
	auto lattice = [&](int x, int y, int z) {
		const int s = (int)kSize;
		x = ((x % s) + s) % s; y = ((y % s) + s) % s; z = ((z % s) + s) % s;
		return hash((UInt32)x, (UInt32)y, (UInt32)z);
	};
	auto sample = [&](F32 x, F32 y, F32 z) {
		const int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
		const F32 fx = x - (F32)xi, fy = y - (F32)yi, fz = z - (F32)zi;
		// Smoothstep for a C1-continuous interpolation between lattice cells.
		const F32 sx = fx * fx * (3.0f - 2.0f * fx);
		const F32 sy = fy * fy * (3.0f - 2.0f * fy);
		const F32 sz = fz * fz * (3.0f - 2.0f * fz);
		F32 acc = 0.0f;
		for (int k = 0; k < 2; ++k) {
			for (int j = 0; j < 2; ++j) {
				for (int i = 0; i < 2; ++i) {
					const F32 w = (i ? sx : 1.0f - sx) * (j ? sy : 1.0f - sy) * (k ? sz : 1.0f - sz);
					acc += w * lattice(xi + i, yi + j, zi + k);
				}
			}
		}
		return acc;
	};

	Vector<UInt8> pixels(static_cast<size_t>(kSize) * kSize * kSize);
	for (UInt32 z = 0; z < kSize; ++z) {
		for (UInt32 y = 0; y < kSize; ++y) {
			for (UInt32 x = 0; x < kSize; ++x) {
				// Three octaves, each at a power-of-two frequency so the result stays tileable.
				F32 v = 0.0f, amp = 1.0f, norm = 0.0f;
				for (int o = 0; o < 3; ++o) {
					const F32 freq = (F32)(4u << o);
					v += amp * sample((F32)x / (F32)kSize * freq, (F32)y / (F32)kSize * freq, (F32)z / (F32)kSize * freq);
					norm += amp;
					amp *= 0.5f;
				}
				v /= norm;
				pixels[(static_cast<size_t>(z) * kSize + y) * kSize + x] = (UInt8)std::clamp(v * 255.0f, 0.0f, 255.0f);
			}
		}
	}

	D::TextureDesc td;
	td.Name = "VolumetricNoise";
	td.Type = D::RESOURCE_DIM_TEX_3D;
	td.Width = td.Height = td.Depth = kSize;
	td.Format = D::TEX_FORMAT_R8_UNORM;
	td.BindFlags = D::BIND_SHADER_RESOURCE;
	td.Usage = D::USAGE_IMMUTABLE;
	td.MipLevels = 1;
	D::TextureSubResData sub;
	sub.pData = pixels.data();
	sub.Stride = kSize;
	sub.DepthStride = kSize * kSize;
	D::TextureData data;
	data.pSubResources = &sub;
	data.NumSubresources = 1;
	tex.Release();
	device->CreateTexture(td, &data, &tex);
	if (!tex) return false;
	srv = tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	return srv != nullptr;
}

} // namespace

VolumetricSubsystem::VolumetricSubsystem() : Subsystem("Volumetric"), m_impl(std::make_unique<Impl>()) {}
VolumetricSubsystem::~VolumetricSubsystem() = default;

void VolumetricSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }

void VolumetricSubsystem::setConfig(const VolumetricConfig& cfg) {
	m_impl->cfg = cfg;
	// The backend is rebuilt lazily on the next render(), so a mode change never happens in
	// the middle of a frame (the backend's output may still be referenced by the composite).
}

const VolumetricConfig& VolumetricSubsystem::config() const { return m_impl->cfg; }

void VolumetricSubsystem::setMode(VolumetricConfig::Mode mode) { m_impl->cfg.mode = mode; }

VolumetricConfig::Mode VolumetricSubsystem::mode() const { return m_impl->cfg.mode; }

const char* VolumetricSubsystem::backendName() const {
	return m_impl->backend ? m_impl->backend->name() : "none";
}

F32 VolumetricSubsystem::lastRenderMs() const {
	const auto& p = *m_impl;
	return (p.backend && p.built) ? p.backend->lastRenderMs() : 0.0f;
}

F32 VolumetricSubsystem::averageRenderMs() const { return m_impl->averageMs; }

F32 VolumetricSubsystem::qualityScale() const { return m_impl->qualityScale; }

void* VolumetricSubsystem::getScatteringSRV() const {
	const auto& p = *m_impl;
	if (!p.ready || !p.built || !p.backend) return nullptr;
	return p.resolvedSRV ? p.resolvedSRV : p.backend->scatteringSRV();
}

Result<void, CoreError> VolumetricSubsystem::onInitialize() {
	auto& p = *m_impl;
	if (!p.renderer) { EError("Volumetric: no renderer attached"); return CoreError::OperationFailed; }
	// Build the configured backend eagerly so a broken shader is reported at startup rather
	// than on the first frame the fog is switched on.
	if (!p.buildBackend(p.cfg.mode)) return CoreError::OperationFailed;
	p.temporal = Detail::createTemporalResolve();
	if (p.temporal && !p.temporal->initialize(p.renderer)) {
		EError("Volumetric: temporal resolve failed to initialize; running without accumulation");
		p.temporal.reset();
	}
	// The medium's noise volume: one texture, shared by both backends (see VolumetricInput).
	if (!createNoiseVolume(static_cast<D::IRenderDevice*>(p.renderer->getDevice()), p.noiseTex, p.noiseSRV)) {
		EWarn("Volumetric: could not create the noise volume; the medium stays smooth");
	}
	p.ready = true;
	return {};
}

void VolumetricSubsystem::onShutdown() {
	auto& p = *m_impl;
	if (p.backend) p.backend->shutdown();
	p.backend.reset();
	if (p.temporal) p.temporal->shutdown();
	p.temporal.reset();
	p.ready = false;
	p.built = false;
	p.resolvedSRV = nullptr;
	p.resetHistory = true;
	p.noiseTex.Release();
	p.noiseSRV.Release();
}

Result<void, CoreError> VolumetricSubsystem::render(const VolumetricInput& input) {
	auto& p = *m_impl;
	if (!p.ready) return CoreError::OperationFailed;
	if (!p.cfg.enabled) {
		p.resolvedSRV = nullptr;
		return {};
	}

	// Rebuild when the mode changed since the backend was created. Doing it here (and not in
	// setMode) keeps the switch at a frame boundary.
	if (!p.built || p.builtMode != p.cfg.mode) {
		if (!p.buildBackend(p.cfg.mode)) return CoreError::OperationFailed;
	}

	// Adaptive quality: the configured step/slice counts are a maximum, scaled down while the
	// pass costs more than the target. A square-root response keeps it from oscillating.
	VolumetricConfig effective = p.cfg;
	if (effective.adaptiveQuality) {
		const F32 s = std::clamp(p.qualityScale, 0.5f, 1.0f);
		effective.steps = std::max<UInt32>(4, (UInt32)((F32)effective.steps * s + 0.5f));
		effective.froxelSlices = std::max<UInt32>(16, (UInt32)((F32)effective.froxelSlices * s + 0.5f));
	}

	// The noise volume belongs to the subsystem, not the caller: hand it over through a copy so
	// the pipeline does not have to know the medium's internals.
	VolumetricInput backendInput = input;
	backendInput.noiseSRV = p.noiseSRV.RawPtr();
	const Result<void, CoreError> result = p.backend->render(backendInput, effective);
	if (result.isErr()) return result;

	// Measure, then decide next frame's budget from the rolling mean. The backend reports the
	// previous frame's GPU time (the query is read one frame late), which is what the average
	// is for - a single frame is far too noisy to steer a governor with.
	const F32 measured = p.backend->lastRenderMs();
	if (measured > 0.0f) {
		p.averageMs = p.averaged ? (p.averageMs * 0.9f + measured * 0.1f) : measured;
		p.averaged = true;
		if (p.cfg.adaptiveQuality && p.cfg.targetMs > 0.0f) {
			if (p.averageMs > p.cfg.targetMs * 1.15f) p.qualityScale = std::max(0.5f, p.qualityScale - 0.05f);
			else if (p.averageMs < p.cfg.targetMs * 0.85f) p.qualityScale = std::min(1.0f, p.qualityScale + 0.02f);
		}
	}

	// Temporal accumulation over the backend's output. Disabling it (or switching backends)
	// resets the history, so the first frame after a change is not a blend of two different
	// algorithms' results.
	p.resolvedSRV = p.backend->scatteringSRV();
	if (p.cfg.temporal && p.temporal) {
		p.resolvedSRV = p.temporal->resolve(p.resolvedSRV, backendInput, p.cfg.temporalWeight, p.resetHistory);
		p.resetHistory = false;
	}
	else {
		p.resetHistory = true;
	}
	// The scene depth was read by the backend and again by the resolve; hand it back in the
	// state the next frame's scene pass expects, once, after the last read. Doing it per pass
	// left the depth in SHADER_RESOURCE whenever the resolve ran, which is exactly the
	// "requires texture 'Depth' to be transitioned to DEPTH_WRITE" error the driver then
	// reports on the following frame.
	if (input.restoreDepthState) {
		auto* context = p.renderer ? static_cast<D::IDeviceContext*>(p.renderer->getContext()) : nullptr;
		auto* view = static_cast<D::ITextureView*>(input.depthSRV);
		if (context && view) {
			if (auto* tex = view->GetTexture()) {
				D::StateTransitionDesc barrier(tex, D::RESOURCE_STATE_SHADER_RESOURCE, D::RESOURCE_STATE_DEPTH_WRITE,
					D::STATE_TRANSITION_FLAG_UPDATE_STATE);
				context->TransitionResourceStates(1, &barrier);
			}
		}
	}
	return {};
}

EE_NAMESPACE_RENDERING_END
