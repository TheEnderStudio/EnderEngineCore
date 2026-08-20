#include <Rendering/DebugUISubsystem.hpp>
#include <Core/Log.hpp>

#ifdef EE_DEBUG

#include <imgui.h>
#include <PostProcess/PostProcessTypes.hpp>
#include <DiligentTools/Imgui/interface/ImGuiImplDiligent.hpp>
#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h>

namespace D = Diligent;

#endif

EE_NAMESPACE_RENDERING_BEGIN

struct DebugUISubsystem::Impl {
#ifdef EE_DEBUG
	std::unique_ptr<D::ImGuiImplDiligent> renderer;
	D::IRenderDevice*  pDevice = nullptr;
	D::IDeviceContext* pCtx    = nullptr;
	bool               ok      = false;
	void*              ppCfgPtr = nullptr;
	bool*              customPtr = nullptr;
	const char**       customShaderSrc = nullptr;
#endif
};

DebugUISubsystem::DebugUISubsystem() : Subsystem("DebugUI"), m_impl(std::make_unique<Impl>()) {}
DebugUISubsystem::~DebugUISubsystem() = default;

void DebugUISubsystem::setDevice(void* d) {
#ifdef EE_DEBUG
	m_impl->pDevice = static_cast<D::IRenderDevice*>(d);
#endif
}
void DebugUISubsystem::setContext(void* c) {
#ifdef EE_DEBUG
	m_impl->pCtx = static_cast<D::IDeviceContext*>(c);
#endif
}
void DebugUISubsystem::setWindowHandle(void*) {}

void DebugUISubsystem::setDebugData(void* cfg, bool* cstm, const char** src) {
#ifdef EE_DEBUG
	m_impl->ppCfgPtr = cfg;
	m_impl->customPtr = cstm;
	m_impl->customShaderSrc = src;
#endif
}

// ===================================================================
// ImGui wrappers (callable from Demo via Event handler)
// ===================================================================

bool DebugUISubsystem::beginWindow(const char* n, bool* o) {
#ifdef EE_DEBUG
	return ImGui::Begin(n, o);
#else
	return false;
#endif
}
void DebugUISubsystem::endWindow() {
#ifdef EE_DEBUG
	ImGui::End();
#endif
}
void DebugUISubsystem::textImpl(const char* str) {
#ifdef EE_DEBUG
	ImGui::TextUnformatted(str);
#endif
}
bool DebugUISubsystem::button(const char* label) {
#ifdef EE_DEBUG
	return ImGui::Button(label);
#else
	return false;
#endif
}
bool DebugUISubsystem::sliderFloat(const char* label, float* v, float min, float max) {
#ifdef EE_DEBUG
	return ImGui::SliderFloat(label, v, min, max);
#else
	return false;
#endif
}
bool DebugUISubsystem::checkbox(const char* label, bool* v) {
#ifdef EE_DEBUG
	return ImGui::Checkbox(label, v);
#else
	return false;
#endif
}

bool DebugUISubsystem::isActive() const {
#ifdef EE_DEBUG
	return m_impl->ok;
#else
	return false;
#endif
}

void DebugUISubsystem::beginFrame(UInt32 w, UInt32 h) {
#ifdef EE_DEBUG
	if (!m_impl->ok) return;
	ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
	m_impl->renderer->NewFrame(w, h, D::SURFACE_TRANSFORM_IDENTITY);

	// Built-in post-process panel
	if (m_impl->ppCfgPtr) {
		auto& cfg = *(EnderEngine::PostProcess::PostProcessConfig*)m_impl->ppCfgPtr;
		bool& custom = *m_impl->customPtr;
		ImGui::Begin("Post-Process Settings");
		ImGui::SliderFloat("Exposure", &cfg.toneMap.exposure, 0.1f, 5.0f);
		ImGui::SliderFloat("Vignette", &cfg.toneMap.vignette, 0.0f, 0.8f);
		ImGui::SliderFloat("Saturation", &cfg.toneMap.saturation, 0.0f, 2.0f);
		ImGui::SliderFloat("Bloom Intensity", &cfg.bloom.intensity, 0.0f, 2.0f);
		ImGui::Checkbox("Bloom Enabled", &cfg.bloom.enabled);
		if (ImGui::Button(custom ? "Shader: CRT Retro" : "Shader: Default")) {
			custom = !custom;
		}
		ImGui::End();
	}

	// Emit event for subscribed objects to draw custom widgets
	DebugUIRenderEvent e; e.width = w; e.height = h;
	emit(e);
#endif
}

void DebugUISubsystem::endFrameAndRender() {
#ifdef EE_DEBUG
	if (!m_impl->ok || !m_impl->pCtx) return;
	m_impl->renderer->Render(m_impl->pCtx);
#endif
}

void DebugUISubsystem::resize(UInt32 w, UInt32 h) {
#ifdef EE_DEBUG
	ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
#endif
}

void DebugUISubsystem::setMousePos(float x, float y) {
#ifdef EE_DEBUG
	if (!m_impl->ok) return;
	ImGui::GetIO().MousePos = ImVec2(x, y);
#endif
}
void DebugUISubsystem::setMouseButton(Int32 b, bool down) {
#ifdef EE_DEBUG
	if (!m_impl->ok) return;
	ImGui::GetIO().MouseDown[b] = down;
#endif
}
void DebugUISubsystem::setMouseWheel(float d) {
#ifdef EE_DEBUG
	if (!m_impl->ok) return;
	ImGui::GetIO().MouseWheel += d;
#endif
}

Result<void, CoreError> DebugUISubsystem::onInitialize() {
#ifdef EE_DEBUG
	if (!m_impl->pDevice || !m_impl->pCtx) {
		EError("DebugUI: missing device/context");
		return CoreError::OperationFailed;
	}
	D::ImGuiDiligentCreateInfo ci(m_impl->pDevice,
		D::TEX_FORMAT_RGBA8_UNORM_SRGB, D::TEX_FORMAT_UNKNOWN);
	m_impl->renderer = std::make_unique<D::ImGuiImplDiligent>(ci);
	ImGui::StyleColorsDark();
	ImGui::GetIO().IniFilename = nullptr;
	m_impl->ok = true;
	EInfo("DebugUI initialized");
	return {};
#else
	return {};
#endif
}

void DebugUISubsystem::onShutdown() {
#ifdef EE_DEBUG
	m_impl->ok = false;
	m_impl->renderer.reset();
	EInfo("DebugUI shut down");
#endif
}

EE_NAMESPACE_RENDERING_END
