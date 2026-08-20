#pragma once

#ifdef __cplusplus
extern "C" {
#endif

	#define EE_EXT_GETEXTPTRFUNCNAME eeExtGetExtensionPtr
	typedef const void* (*eeExtGetExtensionPtrFuncPtr)();

#ifdef __cplusplus
}

#include "Types.hpp"

EE_NAMESPACE_BEGIN
inline constexpr const char* GetExtPtrFuncNameStr = "eeExtGetExtensionPtr";
EE_NAMESPACE_END

#else

const char* EE_API eeGetExtPtrFuncNameStr = "eeExtGetExtensionPtr";

#endif