#include <Core/Crash.h>

#include <Windows.h>
#include <eh.h>
#include <exception>

extern "C" char g_crashReason[4096];

EE_NAMESPACE_BEGIN

static void CppToSehTranslator(unsigned int code, EXCEPTION_POINTERS* ep) {
    EE_UNUSED(code);
    EE_UNUSED(ep);

    const char* whatMsg = "Unknown C++ exception";
    try {
        throw;
    }
    catch (const std::exception& e) {
        whatMsg = e.what();
    }
    catch (...) {}

    strncpy(g_crashReason, whatMsg, sizeof(g_crashReason) - 1);
    g_crashReason[sizeof(g_crashReason) - 1] = '\0';

    ULONG_PTR param = (ULONG_PTR)g_crashReason;
    RaiseException(EE_EXCEPTION_CRASH_CPP, EXCEPTION_NONCONTINUABLE, 1, &param);
}

void CrashHandlerInstallCppTranslator(void) {
    _set_se_translator(CppToSehTranslator);
}

EE_NAMESPACE_END