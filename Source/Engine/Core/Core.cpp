#include <Core/Core.hpp>
#include <Core/Log.hpp>
#include <Core/Crash.h>
#include <Core/Subsystem.hpp>
#include <Core/Extension.hpp>

#include <algorithm>

EE_NAMESPACE_BEGIN

namespace {

bool g_initialized = false;
Vector<Subsystem*> g_subsystems;
Mutex g_subsystemMutex;

} // anonymous namespace

Result<void, CoreError> Engine::initialize() {
	if (g_initialized) {
		EWarn("Engine::initialize() called but engine is already initialized.");
		return CoreError::AlreadyInitialized;
	}

	if (!Log::initialize()) {
		EError("Failed to initialize logging subsystem.");
		return CoreError::OperationFailed;
	}

	EInfo("Engine core initialization started.");

	eeCrashHandlerInstall();
	CrashHandlerInstallCppTranslator();

	ExtensionLoader::initialize();

	g_initialized = true;
	EInfo("Engine core initialized successfully.");
	EInfo("Engine version: {}", getVersion().toString());
	return {};
}

void Engine::shutdown() {
	if (!g_initialized) {
		return;
	}

	EInfo("Engine core shutdown started.");

	{
		std::lock_guard<Mutex> lock(g_subsystemMutex);
		for (auto it = g_subsystems.rbegin(); it != g_subsystems.rend(); ++it) {
			Subsystem* subsystem = *it;
			if (subsystem && subsystem->state() != SubsystemState::Shutdown) {
				subsystem->shutdown();
			}
		}
		g_subsystems.clear();
	}

	ExtensionLoader::shutdown();
	eeCrashHandlerUninstall();
	Log::shutdown();

	g_initialized = false;
	EInfo("Engine core shut down successfully.");
}

bool Engine::isInitialized() {
	return g_initialized;
}

Result<void, CoreError> Engine::registerSubsystem(Subsystem* subsystem) {
	if (!subsystem) {
		EError("Cannot register null subsystem.");
		return CoreError::InvalidArgument;
	}

	std::lock_guard<Mutex> lock(g_subsystemMutex);

	auto it = std::find(g_subsystems.begin(), g_subsystems.end(), subsystem);
	if (it != g_subsystems.end()) {
		EWarn("Subsystem '{}' is already registered.", subsystem->name());
		return CoreError::ObjectAlreadyRegistered;
	}

	g_subsystems.push_back(subsystem);
	EInfo("Subsystem '{}' registered.", subsystem->name());
	return {};
}

void Engine::unregisterSubsystem(Subsystem* subsystem) {
	if (!subsystem) {
		return;
	}

	std::lock_guard<Mutex> lock(g_subsystemMutex);

	auto it = std::find(g_subsystems.begin(), g_subsystems.end(), subsystem);
	if (it == g_subsystems.end()) {
		EWarn("Subsystem '{}' is not registered.", subsystem->name());
		return;
	}

	if ((*it)->state() != SubsystemState::Shutdown) {
		(*it)->shutdown();
	}

	g_subsystems.erase(it);
	EInfo("Subsystem '{}' unregistered.", subsystem->name());
}

Vector<Subsystem*> Engine::subsystems() {
	std::lock_guard<Mutex> lock(g_subsystemMutex);
	return g_subsystems;
}

void Engine::update(F64 deltaTime) {
	if (!g_initialized) {
		return;
	}

	// Snapshot the affected subsystems under the lock, then invoke their
	// update callbacks outside of it so that subsystems may safely register
	// or unregister other subsystems (and Engine::registerSubsystem/unregisterSubsystem
	// may be called) without deadlocking on the non-reentrant registry mutex.
	Vector<Subsystem*> snapshot;
	{
		std::lock_guard<Mutex> lock(g_subsystemMutex);
		snapshot.reserve(g_subsystems.size());
		for (Subsystem* subsystem : g_subsystems) {
			if (subsystem && subsystem->updateMode() == SubsystemUpdateMode::FixedMainThread) {
				snapshot.push_back(subsystem);
			}
		}
	}
	for (Subsystem* subsystem : snapshot) {
		subsystem->update(deltaTime);
	}
}

void Engine::fixedUpdate(F64 fixedDeltaTime) {
	if (!g_initialized) {
		return;
	}

	Vector<Subsystem*> snapshot;
	{
		std::lock_guard<Mutex> lock(g_subsystemMutex);
		snapshot.reserve(g_subsystems.size());
		for (Subsystem* subsystem : g_subsystems) {
			if (subsystem && subsystem->updateMode() == SubsystemUpdateMode::FixedMainThread) {
				snapshot.push_back(subsystem);
			}
		}
	}
	for (Subsystem* subsystem : snapshot) {
		subsystem->fixedUpdate(fixedDeltaTime);
	}
}

const Version& Engine::getVersion() {
	return Version::getEngineVersion();
}

EE_NAMESPACE_END
