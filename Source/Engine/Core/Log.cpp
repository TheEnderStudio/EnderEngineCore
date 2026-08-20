#include <Engine/Core/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#ifdef EE_WINDOWS
# include <spdlog/sinks/msvc_sink.h>
#endif

#include <filesystem>
#include <mutex>

EE_NAMESPACE_BEGIN

namespace {

constexpr const char* g_loggerName = "EnderEngine";
constexpr const char* g_logDirectory = "Logs";
constexpr const char* g_logFilename = "Logs/EnderEngine.log";
constexpr Size g_maxFileSize = 10 * 1024 * 1024;  ///< 10 MB per file.
constexpr Size g_maxFileCount = 5;

Sptr<spdlog::logger> g_logger = nullptr;
std::mutex g_logMutex{};

bool createLogDirectory() {
	std::error_code ec;
	std::filesystem::create_directories(g_logDirectory, ec);
	return !ec;
}

} // anonymous namespace

namespace Log {

bool initialize() {
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_logger) {
		return false;
	}

	if (!createLogDirectory()) {
		return false;
	}

	Vector<Sptr<spdlog::sinks::sink>> sinks;

	// Console sink with colors.
	auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	consoleSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%^%l%$] [%s:%#] [%!] %v");
	sinks.push_back(consoleSink);

#ifdef EE_WINDOWS
	// Visual Studio debugger output sink.
	auto msvcSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
	msvcSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] [%s:%#] [%!] %v");
	sinks.push_back(msvcSink);
#endif

	// Rotating file sink (size-based rotation).
	auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(g_logFilename, g_maxFileSize, g_maxFileCount);
	fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] [%s:%#] [%!] %v");
	sinks.push_back(fileSink);

	g_logger = std::make_shared<spdlog::logger>(g_loggerName, sinks.begin(), sinks.end());
	g_logger->set_level(spdlog::level::trace);
	g_logger->flush_on(spdlog::level::warn);

	spdlog::register_logger(g_logger);
	return true;
}

void shutdown() {
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_logger) {
		g_logger->flush();
		spdlog::drop(g_loggerName);
		g_logger.reset();
	}
}

bool isInitialized() {
	std::lock_guard<std::mutex> lock(g_logMutex);
	return g_logger != nullptr;
}

Sptr<spdlog::logger> getLogger() {
	std::lock_guard<std::mutex> lock(g_logMutex);
	return g_logger;
}

void setLevel(spdlog::level::level_enum level) {
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_logger) {
		g_logger->set_level(level);
	}
}

} // namespace Log

EE_NAMESPACE_END
