#pragma once
/**
 * @file RenderSelfCheck.hpp
 * @brief Startup validation of shader bindings and render resources.
 *
 * Every rendering bug of the last few rounds had the same shape: a feature was enabled, the
 * image did not change, and the only symptom was either a stream of Diligent errors at draw
 * time or nothing at all. The concrete causes were
 *
 *   1. a per-frame texture that was not declared MUTABLE in the PSO's resource layout, so
 *      `IShaderResourceBinding::GetVariableByName()` did not return it and the `Set()` in the
 *      frame/material code silently did nothing (the `t_AO` and `g_Fog` cases);
 *   2. a resource whose view did not match the shader declaration - a multisampled texture
 *      bound to a `Texture2D` variable;
 *   3. a resource that was never created, because one of the two code paths building it
 *      forgot a bind flag (the depth SRV dropped by `RenderSubsystem::resize()`).
 *
 * This turns all three into one line at startup: after a PSO and its binding exist, walk the
 * shaders' resource lists, look every name up in the binding, and report the ones that are
 * not set. It costs one pass over a few dozen names, once.
 *
 * The backend stays hidden: pipeline states, bindings and shaders are passed as opaque
 * handles, exactly like the rest of the rendering API.
 *
 * Enable or disable with @ref EE_RENDER_SELF_CHECK; when it is 0 the calls below are inline
 * no-ops and the implementation compiles to nothing.
 */
#include <Engine/Core/Types.hpp>

/**
 * @brief Enables the startup binding/resource validation (see the file comment).
 *
 * 1 by default in debug builds (EE_DEBUG is defined by the build system) and 0 otherwise.
 * Define it to 0 before including this header to compile the checks out of a debug build, or
 * to 1 to keep them in a release build.
 */
#ifndef EE_RENDER_SELF_CHECK
#	ifdef EE_DEBUG
#		define EE_RENDER_SELF_CHECK 1
#	else
#		define EE_RENDER_SELF_CHECK 0
#	endif
#endif

EE_NAMESPACE_RENDERING_BEGIN

#if EE_RENDER_SELF_CHECK

/**
 * @brief Verifies that every resource declared by @p shaders is bound in @p binding.
 *
 * A name the binding resolves must have a value set; a name the binding does not hold is
 * either a static variable of the pipeline state (checked there) or absent from the resource
 * layout entirely - which is the "declared in the shader but never listed as MUTABLE" bug.
 *
 * @param label         Name used in the report, typically the PSO name.
 * @param pipelineState The pipeline state the binding was created from (may be null).
 * @param binding       The shader resource binding to inspect.
 * @param shaders       Shader handles to enumerate resources from (null entries are skipped).
 * @param count         Number of entries in @p shaders.
 * @return true when every declared resource is bound.
 */
EE_API bool checkShaderBindings(const char* label, void* pipelineState, void* binding, void* const* shaders, UInt32 count);

/**
 * @brief Reports a required resource that turned out to be unavailable.
 *
 * Meant for resources a feature depends on but nobody owns end to end - the scene depth SRV
 * the volumetric pass needs, for example - so a missing one is named once instead of the
 * feature silently doing nothing every frame.
 */
EE_API bool checkResourceAvailable(const char* feature, const char* resource, const void* handle);

/// @brief Number of problems reported so far (0 when everything checked out).
EE_NODISCARD EE_API UInt32 renderSelfCheckFailures();

/// @brief Logs a one-line summary; call once after the first frame has bound everything.
EE_API void renderSelfCheckSummary();

#else // EE_RENDER_SELF_CHECK

inline bool checkShaderBindings(const char*, void*, void*, void* const*, UInt32) { return true; }
inline bool checkResourceAvailable(const char*, const char*, const void*) { return true; }
inline UInt32 renderSelfCheckFailures() { return 0; }
inline void renderSelfCheckSummary() {}

#endif // EE_RENDER_SELF_CHECK

EE_NAMESPACE_RENDERING_END
