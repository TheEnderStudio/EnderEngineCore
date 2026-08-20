#include <Core/Crash.h>

#pragma warning ( disable: 4996 )

#ifndef _CRT_SECURE_NO_WARNINGS
# define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")

static LPTOP_LEVEL_EXCEPTION_FILTER g_oldFilter = NULL;
char g_crashReason[4096] = { 0 };
static CRITICAL_SECTION g_cs;

static void WriteCrashReport(const EXCEPTION_POINTERS* ep, const char* reason);
static void ShowCrashDialog(const char* brief);
static void GetSystemInfoString(char* buffer, size_t size);
static void GetStackTrace(const EXCEPTION_POINTERS* ep, char* stackBuf, size_t size);
static void InitSymbols(void);

static void GetTimeString(char* buf, size_t size) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
}

static DWORD GetCurrentThreadIdWrapper(void) {
    return GetCurrentThreadId();
}

static void GetSystemInfoString(char* buffer, size_t size) {
    char cpuName[64] = "Unknown";
    char vendor[16] = "Unknown";
    int cpuInfo[4] = { 0 };
    MEMORYSTATUSEX mem = { sizeof(mem) };
    GlobalMemoryStatusEx(&mem);

    __cpuid(cpuInfo, 0);
    memcpy(vendor, &cpuInfo[1], 4);
    memcpy(vendor + 4, &cpuInfo[3], 4);
    memcpy(vendor + 8, &cpuInfo[2], 4);
    vendor[12] = '\0';

    __cpuid(cpuInfo, 0x80000002);
    memcpy(cpuName, cpuInfo, 16);
    __cpuid(cpuInfo, 0x80000003);
    memcpy(cpuName + 16, cpuInfo, 16);
    __cpuid(cpuInfo, 0x80000004);
    memcpy(cpuName + 32, cpuInfo, 16);
    cpuName[48] = '\0';

    OSVERSIONINFOEXW osvi = { sizeof(osvi) };
    if (!GetVersionExW((OSVERSIONINFOW*)&osvi)) {
        osvi.dwMajorVersion = 10;
        osvi.dwMinorVersion = 0;
        wcscpy(osvi.szCSDVersion, L"");
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);

    snprintf(buffer, size,
        "CPU Vendor: %s\n"
        "CPU Name: %s\n"
        "CPU Cores: %u\n"
        "Memory Total: %llu MB\n"
        "Memory Available: %llu MB\n"
        "OS: Windows %lu.%lu (Build %lu) %S\n"
        "Architecture: %s\n",
        vendor,
        cpuName,
        si.dwNumberOfProcessors,
        (unsigned long long)mem.ullTotalPhys / (1024 * 1024),
        (unsigned long long)mem.ullAvailPhys / (1024 * 1024),
        osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber,
        osvi.szCSDVersion,
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" :
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "Other");
}

static void InitSymbols(void) {
    static int initialized = 0;
    if (!initialized) {
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        initialized = 1;
    }
}

static void GetStackTrace(const EXCEPTION_POINTERS* ep, char* stackBuf, size_t size) {
    InitSymbols();

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    CONTEXT ctx = *ep->ContextRecord;
    
    STACKFRAME64 sf = { 0 };
    DWORD machine = 0;
#ifdef _M_X64
    machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
#elif _M_IX86
    machine = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset = ctx.Eip;
    sf.AddrFrame.Offset = ctx.Ebp;
    sf.AddrStack.Offset = ctx.Esp;
#else
#error "Unsupported platform"
#endif
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;

    char line[1024];
    size_t totalLen = 0;
    stackBuf[0] = '\0';

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, process, thread, &sf,
            &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;

        if (sf.AddrPC.Offset == 0)
            break;

        DWORD64 displacement = 0;
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256, 1);
        symbol->MaxNameLen = 255;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        IMAGEHLP_LINE64 lineInfo = { sizeof(IMAGEHLP_LINE64) };
        DWORD lineDisplacement = 0;
        const char* funcName = "<Unknown Function>";
        const char* fileName = "<Unknown File>";
        int lineNum = 0;

        if (SymFromAddr(process, sf.AddrPC.Offset, &displacement, symbol))
            funcName = symbol->Name;

        if (SymGetLineFromAddr64(process, sf.AddrPC.Offset, &lineDisplacement, &lineInfo)) {
            fileName = lineInfo.FileName;
            lineNum = lineInfo.LineNumber;
        }

        snprintf(line, sizeof(line),
            "#%02d 0x%016llX  %s()  at %s:%d\n",
            i, sf.AddrPC.Offset, funcName, fileName, lineNum);

        size_t len = strlen(line);
        if (totalLen + len < size) {
            strcat(stackBuf, line);
            totalLen += len;
        }

        free(symbol);
    }
}

static void WriteCrashReport(const EXCEPTION_POINTERS* ep, const char* reason) {
    char timeStr[64];
    GetTimeString(timeStr, sizeof(timeStr));

    char sysInfo[4096];
    GetSystemInfoString(sysInfo, sizeof(sysInfo));

    char stackTrace[8192] = { 0 };
    GetStackTrace(ep, stackTrace, sizeof(stackTrace));

    char threadIdStr[32];
    snprintf(threadIdStr, sizeof(threadIdStr), "%lu", GetCurrentThreadIdWrapper());

    char excDesc[256];
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char* codeStr = "Unknown";
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: codeStr = "Access Violation"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: codeStr = "Array Bounds Exceeded"; break;
    case EXCEPTION_BREAKPOINT: codeStr = "Breakpoint"; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: codeStr = "Datatype Misalignment"; break;
    case EXCEPTION_FLT_DENORMAL_OPERAND: codeStr = "Float Denormal Operand"; break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: codeStr = "Float Divide by Zero"; break;
    case EXCEPTION_FLT_INEXACT_RESULT: codeStr = "Float Inexact Result"; break;
    case EXCEPTION_FLT_INVALID_OPERATION: codeStr = "Float Invalid Operation"; break;
    case EXCEPTION_FLT_OVERFLOW: codeStr = "Float Overflow"; break;
    case EXCEPTION_FLT_STACK_CHECK: codeStr = "Float Stack Check"; break;
    case EXCEPTION_FLT_UNDERFLOW: codeStr = "Float Underflow"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: codeStr = "Illegal Instruction"; break;
    case EXCEPTION_IN_PAGE_ERROR: codeStr = "In Page Error"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: codeStr = "Integer Divide by Zero"; break;
    case EXCEPTION_INT_OVERFLOW: codeStr = "Integer Overflow"; break;
    case EXCEPTION_INVALID_DISPOSITION: codeStr = "Invalid Disposition"; break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: codeStr = "Noncontinuable Exception"; break;
    case EXCEPTION_PRIV_INSTRUCTION: codeStr = "Privileged Instruction"; break;
    case EXCEPTION_STACK_OVERFLOW: codeStr = "Stack Overflow"; break;
    case EE_EXCEPTION_CRASH_MANUAL: codeStr = "Manual Trigger"; break;
    case EE_EXCEPTION_CRASH_CPP: codeStr = "C++ Exception"; break;
    default: codeStr = "Unknown Exception"; break;
    }
    snprintf(excDesc, sizeof(excDesc), "%s (0x%08X)", codeStr, code);

    char report[16384];
    snprintf(report, sizeof(report),
        "================== CRASH REPORT ==================\n\n"
        "Time: %s\n"
        "Thread ID: %s\n"
        "Exception: %s\n"
        "Reason: %s\n"
        "\n--- Stack Trace ---\n%s\n\n"
        "--- System Information ---\n%s\n\n"
        "==================================================\n",
        timeStr, threadIdStr, excDesc,
        reason ? reason : "No additional reason provided.",
        stackTrace, sysInfo);

    CreateDirectoryA("Crash", NULL);

    char filename[256];
    snprintf(filename, sizeof(filename), "Crash/Crash_Report_%s.txt", timeStr);
    for (char* p = filename; *p; ++p) {
        if (*p == ':' || *p == ' ') *p = '_';
    }

    FILE* f = fopen(filename, "w");
    if (f) {
        fwrite(report, 1, strlen(report), f);
        fclose(f);
    }
}

static void ShowCrashDialog(const char* brief) {
    MessageBoxA(NULL, brief, "Ender Engine Crash Handler", MB_OK | MB_ICONERROR);
}

static LONG WINAPI UnhandledExceptionFilterProc(EXCEPTION_POINTERS* ep) {
    const char* reason = "No reason provided.";
    if (ep->ExceptionRecord->ExceptionCode == EE_EXCEPTION_CRASH_MANUAL ||
        ep->ExceptionRecord->ExceptionCode == EE_EXCEPTION_CRASH_CPP) {
        if (ep->ExceptionRecord->NumberParameters >= 1) {
            const char* ptr = (const char*)ep->ExceptionRecord->ExceptionInformation[0];
            if (ptr && IsBadReadPtr(ptr, 1) == 0) {
                reason = ptr;
            }
        }
    }

    char brief[1024];
    char timeStr[64];
    GetTimeString(timeStr, sizeof(timeStr));
    snprintf(brief, sizeof(brief),
        "A crash has occurred!\n\n"
        "Time: %s\n"
        "Thread ID: %lu\n"
        "Exception: 0x%08X\n"
        "Reason: %s\n\n"
        "Full report saved to Crash/ folder.",
        timeStr, GetCurrentThreadIdWrapper(),
        ep->ExceptionRecord->ExceptionCode,
        reason);

    ShowCrashDialog(brief);
    WriteCrashReport(ep, reason);
    return EXCEPTION_EXECUTE_HANDLER;
}

void eeCrashHandlerInstall(void) {
    InitializeCriticalSection(&g_cs);
    g_oldFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterProc);
}

void eeCrashHandlerUninstall(void) {
    if (g_oldFilter) {
        SetUnhandledExceptionFilter(g_oldFilter);
        g_oldFilter = NULL;
    }
    DeleteCriticalSection(&g_cs);
}

void eeCrashHandlerTrigger(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    strncpy(g_crashReason, buffer, sizeof(g_crashReason) - 1);
    g_crashReason[sizeof(g_crashReason) - 1] = '\0';

    ULONG_PTR param = (ULONG_PTR)g_crashReason;
    RaiseException(EE_EXCEPTION_CRASH_MANUAL, EXCEPTION_NONCONTINUABLE, 1, &param);
}