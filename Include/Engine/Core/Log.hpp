#pragma once

#include "Macros.h"
#include "Types.hpp"

#include <spdlog/spdlog.h>

EE_NAMESPACE_BEGIN

/**
 * @brief Logging subsystem wrapping spdlog.
 *
 * The global logger outputs to console (colored), Visual Studio debugger,
 * and a rotating log file. Use the ETrace/EDebug/EInfo/EWarn/EError/ECritical
 * macros so that file, line and function information are captured automatically.
 */
namespace Log {

/**
 * @brief Initialize the global logger with all sinks.
 * @return true on success, false if already initialized or failed.
 */
EE_API bool initialize();

/**
 * @brief Shut down the global logger and flush pending messages.
 */
EE_API void shutdown();

/**
 * @brief Check whether the logging subsystem has been initialized.
 * @return true if initialized.
 */
EE_API bool isInitialized();

/**
 * @brief Get the underlying spdlog logger.
 * @return Shared pointer to the global logger, or nullptr if not initialized.
 */
EE_API Sptr<spdlog::logger> getLogger();

/**
 * @brief Set the minimum log level.
 * @param level The desired spdlog log level.
 */
EE_API void setLevel(spdlog::level::level_enum level);

/**
 * @brief Log a trace message.
 * @tparam Args Format argument types.
 * @param file Source file path.
 * @param line Source line number.
 * @param function Function name.
 * @param fmt Format string (fmt style).
 * @param args Format arguments.
 */
template <typename... Args>
void trace(const char* file, int line, const char* function, spdlog::format_string_t<Args...> fmt, Args&&... args) {
	auto logger = getLogger();
	if (logger) {
		logger->log(spdlog::source_loc{file, line, function}, spdlog::level::trace, fmt, std::forward<Args>(args)...);
	}
}

/**
 * @brief Log a debug message.
 * @tparam Args Format argument types.
 * @param file Source file path.
 * @param line Source line number.
 * @param function Function name.
 * @param fmt Format string (fmt style).
 * @param args Format arguments.
 */
template <typename... Args>
void debug(const char* file, int line, const char* function, spdlog::format_string_t<Args...> fmt, Args&&... args) {
	auto logger = getLogger();
	if (logger) {
		logger->log(spdlog::source_loc{file, line, function}, spdlog::level::debug, fmt, std::forward<Args>(args)...);
	}
}

/**
 * @brief Log an info message.
 * @tparam Args Format argument types.
 * @param file Source file path.
 * @param line Source line number.
 * @param function Function name.
 * @param fmt Format string (fmt style).
 * @param args Format arguments.
 */
template <typename... Args>
void info(const char* file, int line, const char* function, spdlog::format_string_t<Args...> fmt, Args&&... args) {
	auto logger = getLogger();
	if (logger) {
		logger->log(spdlog::source_loc{file, line, function}, spdlog::level::info, fmt, std::forward<Args>(args)...);
	}
}

/**
 * @brief Log a warning message.
 * @tparam Args Format argument types.
 * @param file Source file path.
 * @param line Source line number.
 * @param function Function name.
 * @param fmt Format string (fmt style).
 * @param args Format arguments.
 */
template <typename... Args>
void warn(const char* file, int line, const char* function, spdlog::format_string_t<Args...> fmt, Args&&... args) {
	auto logger = getLogger();
	if (logger) {
		logger->log(spdlog::source_loc{file, line, function}, spdlog::level::warn, fmt, std::forward<Args>(args)...);
	}
}

/**
 * @brief Log an error message.
 * @tparam Args Format argument types.
 * @param file Source file path.
 * @param line Source line number.
 * @param function Function name.
 * @param fmt Format string (fmt style).
 * @param args Format arguments.
 */
template <typename... Args>
void error(const char* file, int line, const char* function, spdlog::format_string_t<Args...> fmt, Args&&... args) {
	auto logger = getLogger();
	if (logger) {
		logger->log(spdlog::source_loc{file, line, function}, spdlog::level::err, fmt, std::forward<Args>(args)...);
	}
}

/**
 * @brief Log a critical message.
 * @tparam Args Format argument types.
 * @param file Source file path.
 * @param line Source line number.
 * @param function Function name.
 * @param fmt Format string (fmt style).
 * @param args Format arguments.
 */
template <typename... Args>
void critical(const char* file, int line, const char* function, spdlog::format_string_t<Args...> fmt, Args&&... args) {
	auto logger = getLogger();
	if (logger) {
		logger->log(spdlog::source_loc{file, line, function}, spdlog::level::critical, fmt, std::forward<Args>(args)...);
	}
}

} // namespace Log

EE_NAMESPACE_END

#ifdef EE_DEBUG
#define ETrace(...)   ::EnderEngine::Log::trace(EE_FILE, EE_LINE, EE_FUNCTION, __VA_ARGS__)
#define EDebug(...)   ::EnderEngine::Log::debug(EE_FILE, EE_LINE, EE_FUNCTION, __VA_ARGS__)
#define EInfo(...)    ::EnderEngine::Log::info(EE_FILE, EE_LINE, EE_FUNCTION, __VA_ARGS__)
#else
#define ETrace(...)
#define EDebug(...)
#define EInfo(...)
#endif
#define EWarn(...)    ::EnderEngine::Log::warn(EE_FILE, EE_LINE, EE_FUNCTION, __VA_ARGS__)
#define EError(...)   ::EnderEngine::Log::error(EE_FILE, EE_LINE, EE_FUNCTION, __VA_ARGS__)
#define ECritical(...) ::EnderEngine::Log::critical(EE_FILE, EE_LINE, EE_FUNCTION, __VA_ARGS__)
