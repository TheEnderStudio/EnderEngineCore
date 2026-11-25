#include <Rendering/RenderSelfCheck.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/ShaderResourceBinding.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>


namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

#if EE_RENDER_SELF_CHECK

namespace {

/// One accumulator for the whole startup: the summary line reports it once.
UInt32 g_failures = 0;
/// Keys already reported, so a check running every frame says each problem once.
HashSet<String> g_reported;

/// @brief Logs @p message the first time @p key is seen; returns false so callers can chain.
bool reportOnce(const String& key, String message) {
	if (!g_reported.insert(key).second) return false;
	++g_failures;
	EError("{}", message);
	return false;
}

} // namespace

bool checkShaderBindings(const char* label, void* pipelineState, void* binding, void* const* shaders, UInt32 count) {
	auto* pso = static_cast<D::IPipelineState*>(pipelineState);
	auto* srb = static_cast<D::IShaderResourceBinding*>(binding);
	if (!pso || !shaders) return true;
	const String owner = label ? label : "<unnamed>";
	if (!srb) {
		return reportOnce(owner + "/binding", fmt::format(
			"SelfCheck: '{}' has no shader resource binding", owner));
	}

	bool ok = true;
	for (UInt32 s = 0; s < count; ++s) {
		auto* shader = static_cast<D::IShader*>(shaders[s]);
		if (!shader) continue;
		const D::SHADER_TYPE stage = shader->GetDesc().ShaderType;

		const D::Uint32 resourceCount = shader->GetResourceCount();
		for (D::Uint32 r = 0; r < resourceCount; ++r) {
			D::ShaderResourceDesc desc;
			shader->GetResourceDesc(r, desc);
			if (!desc.Name) continue;

			const String key = fmt::format("{}/{}", owner, desc.Name);
			// Immutable samplers are described by the pipeline state's layout, so they are in
			// neither the binding nor the statics - not a problem, which is why a sampler the
			// binding does not hold is skipped rather than reported.
			const bool isSampler = desc.Type == D::SHADER_RESOURCE_TYPE_SAMPLER;
			// A mutable variable lives in the binding; a static one belongs to the pipeline
			// state. Whichever table holds it, the value has to be set before the first draw.
			if (auto* var = srb->GetVariableByName(stage, desc.Name)) {
				if (var->Get() == nullptr) {
					ok = reportOnce(key, fmt::format(
						"SelfCheck: '{}' declares '{}' but its binding slot is empty - a mutable "
						"variable that is never Set() samples whatever was in that descriptor",
						owner, desc.Name));
				}
			}
			else if (auto* staticVar = pso->GetStaticVariableByName(stage, desc.Name)) {
				if (staticVar->Get() == nullptr) {
					ok = reportOnce(key, fmt::format(
						"SelfCheck: '{}' declares '{}' as a static variable that was never initialised",
						owner, desc.Name));
				}
			}
			else if (isSampler) {
				// An immutable sampler: the layout owns it, nothing to bind.
			}
			else {
				// Neither table has it: the layout never listed this name, so it falls back to
				// the layout's default variable type. With a STATIC default the per-frame Set()
				// looks the name up through the binding, finds nothing and silently does
				// nothing - the t_AO / g_Fog bug.
				ok = reportOnce(key, fmt::format(
					"SelfCheck: '{}' declares '{}', but it is in neither the binding nor the "
					"pipeline state's statics - it is missing from the resource layout "
					"(declare it MUTABLE)",
					owner, desc.Name));
			}
		}
	}
	return ok;
}

bool checkResourceAvailable(const char* feature, const char* resource, const void* handle) {
	if (handle) return true;
	const String name = resource ? resource : "<unnamed resource>";
	return reportOnce(fmt::format("resource/{}", name), fmt::format(
		"SelfCheck: {} is unavailable, so {} is disabled until it exists",
		name, feature ? feature : "<unnamed feature>"));
}

UInt32 renderSelfCheckFailures() { return g_failures; }

void renderSelfCheckSummary() {
	if (g_failures == 0) {
		EInfo("SelfCheck: every shader binding and required resource is valid");
		return;
	}
	EError("SelfCheck: {} problem(s) found - the features that depend on them are disabled or "
		"bind the wrong resource (see the SelfCheck lines above)", g_failures);
}

#else // EE_RENDER_SELF_CHECK

// The header's inline stubs cover every call when the checks are disabled.

#endif // EE_RENDER_SELF_CHECK

EE_NAMESPACE_RENDERING_END
