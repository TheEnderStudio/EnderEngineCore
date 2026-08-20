#pragma once

#include "Macros.h"

#define EE_EXCEPTION_CRASH_MANUAL  0xE0000001
#define EE_EXCEPTION_CRASH_CPP     0xE06D7363

#ifdef __cplusplus
extern "C" {
#endif

	void EE_API eeCrashHandlerInstall(void);

	void EE_API eeCrashHandlerTrigger(const char* format, ...);

	void EE_API eeCrashHandlerUninstall(void);

#ifdef __cplusplus
}
EE_NAMESPACE_BEGIN

void EE_API CrashHandlerInstallCppTranslator(void);

EE_NAMESPACE_END
#endif

#define ECrash(fmt, ...) { eeCrashHandlerTrigger(fmt, __VA_ARGS__); }
#define EAssert(cond) { if (!cond) ECrash("Assertion Failed: %s", #cond); }
#if EE_DEBUG
#define EAssertD(cond) EAssert(cond)
#else
#define EAssertD(cond)
#endif