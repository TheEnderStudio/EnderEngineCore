#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

#ifdef _DEBUG
# include <crtdbg.h>
#endif

namespace {

/// @brief Path of the diagnostic log, emptied once per run.
const char* logPath() {
	static char s_path[512] = {};
	if (s_path[0] == 0) {
		const char* tmp = std::getenv("TEMP");
		std::snprintf(s_path, sizeof(s_path), "%s\\EnderEngineTestDiag.log", tmp ? tmp : ".");
	}
	return s_path;
}

/// @brief Append a line to the diagnostic log.
///
/// Used by the crash/abort hooks below: a failing test that aborts leaves nothing on
/// stderr in a console run, and a silent exit code 3 is not diagnosable.
void logLine(const char* what) {
	if (FILE* f = std::fopen(logPath(), "a")) {
		std::fprintf(f, "%s\n", what);
		std::fclose(f);
	}
}

void logAndAbort(const char* what) {
	logLine(what);
	std::abort();
}

#ifdef _DEBUG
void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
	logAndAbort("CRT invalid parameter");
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _DEBUG
	// CRT debug-heap assertions go to the debugger by default, so in a console run they
	// are invisible; route them to the log file (and to stderr for good measure).
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_set_invalid_parameter_handler(&invalidParameterHandler);
#endif

	// A terminate with no message is the least diagnosable failure there is; record it.
	std::set_terminate([] {
		if (auto eptr = std::current_exception()) {
			try {
				std::rethrow_exception(eptr);
			}
			catch (const std::bad_alloc&) { logAndAbort("terminate: std::bad_alloc"); }
			catch (const std::exception& e) { logLine(e.what()); logAndAbort("terminate: std::exception"); }
			catch (...) { logAndAbort("terminate: unknown exception"); }
		}
		logAndAbort("terminate: no active exception (noexcept violation or abort())");
	});

	// Start every run with an empty log so a stale line cannot be mistaken for this run's.
	std::remove(logPath());

	testing::InitGoogleTest(&argc, argv);
	printf("Running all tests ...");
	std::fflush(stdout);
	const int result = RUN_ALL_TESTS();
	printf("\n");
	return result;
}
