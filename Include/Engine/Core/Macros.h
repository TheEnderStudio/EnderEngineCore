#pragma once

#ifdef EE_WINDOWS
# ifdef EE_EXPORTS
#  define EE_API __declspec(dllexport)
# else
#  define EE_API __declspec(dllimport)
# endif
#else
# define EE_API __attribute__((visibility("default")))
#endif

#define EE_NO_COPY(ClassName) \
	ClassName(const ClassName&) = delete; \
	ClassName& operator=(const ClassName&) = delete;

#define EE_NO_MOVE(ClassName) \
	ClassName(ClassName&&) = delete; \
	ClassName& operator=(ClassName&&) = delete;

#define EE_DEFAULT_COPY(ClassName) \
	ClassName(const ClassName&) = default; \
	ClassName& operator=(const ClassName&) = default;

#define EE_DEFAULT_MOVE(ClassName) \
	ClassName(ClassName&&) = default; \
	ClassName& operator=(ClassName&&) = default;

#define EE_DEFAULT_CON_DES(ClassName) \
	ClassName() = default; \
	~ClassName() = default;

#define EE_STRINGFY(str) #str
#define EE_CONCAT(a, b) a##b

#define EE_UNUSED(a) (void)(a)

#define EE_DEPRECATED(reason) [[deprecated(reason)]]

#define EE_NODISCARD [[nodiscard]]
#define EE_CONSTEXPR constexpr

#if defined(__GNUC__) || defined(__clang__)
# define EE_LIKELY(x) __builtin_expect(!!(x), 1)
# define EE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
# define EE_LIKELY(x) (x)
# define EE_UNLIKELY(x) (x)
#endif

#define EE_FILE __FILE__
#define EE_LINE __LINE__
#define EE_FUNCTION __FUNCTION__

#define EE_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define EE_OFFSET_OF(Type, Member) offsetof(Type, Member)

#define EE_NAMESPACE ::EnderEngine::
#define EE_NAMESPACE_BEGIN namespace EnderEngine {
#define EE_NAMESPACE_END }
#define EE_NAMESPACE_AUDIO_BEGIN namespace EnderEngine::Audio {
#define EE_NAMESPACE_AUDIO_END }
#define EE_NAMESPACE_RENDERING_BEGIN namespace EnderEngine::Rendering {
#define EE_NAMESPACE_RENDERING_END }
#define EE_NAMESPACE_PLATFORM_BEGIN namespace EnderEngine::Platform {
#define EE_NAMESPACE_PLATFORM_END }
#define EE_NAMESPACE_INPUT_BEGIN namespace EnderEngine::Input {
#define EE_NAMESPACE_INPUT_END }
#define EE_NAMESPACE_JOBS_BEGIN namespace EnderEngine::Jobs {
#define EE_NAMESPACE_JOBS_END }
#define EE_NAMESPACE_UTILITIES_BEGIN namespace EnderEngine::Utilities {
#define EE_NAMESPACE_UTILITIES_END }
#define EE_NAMESPACE_POSTPROCESS_BEGIN namespace EnderEngine::PostProcess {
#define EE_NAMESPACE_POSTPROCESS_END }
#define EE_NAMESPACE_UI_BEGIN namespace EnderEngine::UI {
#define EE_NAMESPACE_UI_END }
#define EE_NAMESPACE_AI_BEGIN namespace EnderEngine::AI {
#define EE_NAMESPACE_AI_END }
#define EE_NAMESPACE_PHYSICS_BEGIN namespace EnderEngine::Physics {
#define EE_NAMESPACE_PHYSICS_END }

#define EE_VERSION_MAJOR 0
#define EE_VERSION_MINOR 1
#define EE_VERSION_PATCH 1
#define EE_VERSION_GUID  "4BDED345-8915-459A-A151-01810E820BF2"