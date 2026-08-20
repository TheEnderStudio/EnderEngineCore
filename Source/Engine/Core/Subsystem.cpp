#include <Engine/Core/Subsystem.hpp>

#include <Engine/Core/Log.hpp>
#include <Engine/Core/Crash.h>

#include <chrono>
#include <thread>

EE_NAMESPACE_BEGIN

namespace {

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<F64>;

} // anonymous namespace

Subsystem::Subsystem(StringView name) : m_name(name) {
}

Subsystem::~Subsystem() {
	if (state() != SubsystemState::Shutdown) {
		shutdown();
	}
}

Result<void, CoreError> Subsystem::initialize() {
	if (state() != SubsystemState::Initialized) {
		return CoreError::AlreadyInitialized;
	}

	auto result = onInitialize();
	if (!result) {
		return result.error();
	}

	setState(SubsystemState::Running);
	EInfo("Subsystem '{}' initialized.", m_name);
	return {};
}

void Subsystem::shutdown() {
	stopThread();

	{
		std::lock_guard<Mutex> lock(m_objectMutex);
		m_eventBus.clear();
		m_objectSubscriptions.clear();
		m_objectToInternal.clear();
		m_internalToObject.clear();
	}

	onShutdown();
	setState(SubsystemState::Shutdown);
	EInfo("Subsystem '{}' shut down.", m_name);
}

Result<void, CoreError> Subsystem::registerObject(const Object& object) {
	if (object.guid().isNil()) {
		return CoreError::InvalidArgument;
	}

	std::lock_guard<Mutex> lock(m_objectMutex);
	if (m_objectToInternal.contains(object.guid())) {
		return CoreError::ObjectAlreadyRegistered;
	}

	const InternalHandle handle = m_nextInternalHandle++;
	m_objectToInternal.emplace(object.guid(), handle);
	m_internalToObject.emplace(handle, object);

	onObjectRegistered(object, handle);
	EDebug("Object {} registered in subsystem '{}' with handle {}.", object.guid().toString(), m_name, handle);
	return {};
}

void Subsystem::unregisterObject(const Object& object) {
	std::lock_guard<Mutex> lock(m_objectMutex);
	auto it = m_objectToInternal.find(object.guid());
	if (it == m_objectToInternal.end()) {
		return;
	}

	const InternalHandle handle = it->second;
	m_internalToObject.erase(handle);
	m_objectToInternal.erase(it);

	auto subIt = m_objectSubscriptions.find(object.guid());
	if (subIt != m_objectSubscriptions.end()) {
		for (SubscriptionId id : subIt->second) {
			m_eventBus.unsubscribe(id);
		}
		m_objectSubscriptions.erase(subIt);
	}

	onObjectUnregistered(object, handle);
	EDebug("Object {} unregistered from subsystem '{}'.", object.guid().toString(), m_name);
}

bool Subsystem::hasObject(const Object& object) const {
	std::lock_guard<Mutex> lock(m_objectMutex);
	return m_objectToInternal.contains(object.guid());
}

UInt32 Subsystem::getInternalHandle(const Object& object) const {
	std::lock_guard<Mutex> lock(m_objectMutex);
	auto it = m_objectToInternal.find(object.guid());
	if (it == m_objectToInternal.end()) {
		return 0;
	}
	return it->second;
}

Optional<Object> Subsystem::getObject(UInt32 handle) const {
	std::lock_guard<Mutex> lock(m_objectMutex);
	auto it = m_internalToObject.find(handle);
	if (it == m_internalToObject.end()) {
		return NullOpt;
	}
	return it->second;
}

void Subsystem::update(F64 deltaTime) {
	if (state() != SubsystemState::Running || m_paused.load(std::memory_order_acquire)) {
		return;
	}
	protectedRun([this, deltaTime]() { onUpdate(deltaTime); });
}

void Subsystem::fixedUpdate(F64 fixedDeltaTime) {
	if (state() != SubsystemState::Running || m_paused.load(std::memory_order_acquire)) {
		return;
	}
	protectedRun([this, fixedDeltaTime]() { onFixedUpdate(fixedDeltaTime); });
}

Result<void, CoreError> Subsystem::startThread() {
	if (m_updateMode == SubsystemUpdateMode::FixedMainThread) {
		return CoreError::InvalidArgument;
	}
	if (m_threadRunning.load(std::memory_order_acquire)) {
		return CoreError::AlreadyInitialized;
	}

	m_threadRunning.store(true, std::memory_order_release);
	try {
		m_updateThread = std::thread(&Subsystem::threadLoop, this);
	}
	catch (const std::system_error&) {
		m_threadRunning.store(false, std::memory_order_release);
		return CoreError::ThreadingError;
	}

	EInfo("Subsystem '{}' update thread started.", m_name);
	return {};
}

void Subsystem::stopThread() {
	if (!m_threadRunning.load(std::memory_order_acquire)) {
		return;
	}

	m_threadRunning.store(false, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(m_pauseMutex);
		m_paused.store(false, std::memory_order_release);
	}
	m_pauseCondition.notify_all();

	if (m_updateThread.joinable()) {
		m_updateThread.join();
	}

	EInfo("Subsystem '{}' update thread stopped.", m_name);
}

void Subsystem::pause() {
	m_paused.store(true, std::memory_order_release);
}

void Subsystem::resume() {
	{
		std::lock_guard<std::mutex> lock(m_pauseMutex);
		m_paused.store(false, std::memory_order_release);
	}
	m_pauseCondition.notify_all();
}

bool Subsystem::tryRecover() {
	EWarn("Subsystem '{}' attempting crash recovery.", m_name);
	if (onRecover()) {
		setState(SubsystemState::Running);
		EInfo("Subsystem '{}' recovered successfully.", m_name);
		return true;
	}
	return false;
}

void Subsystem::threadLoop() {
	auto previousTime = Clock::now();

	while (m_threadRunning.load(std::memory_order_acquire)) {
		// Handle pause.
		{
			std::unique_lock<std::mutex> lock(m_pauseMutex);
			m_pauseCondition.wait(lock, [this]() {
				return !m_paused.load(std::memory_order_acquire) || !m_threadRunning.load(std::memory_order_acquire);
			});
		}

		if (!m_threadRunning.load(std::memory_order_acquire)) {
			break;
		}

		auto currentTime = Clock::now();
		F64 deltaTime = std::chrono::duration_cast<Duration>(currentTime - previousTime).count();
		previousTime = currentTime;

		if (m_updateMode == SubsystemUpdateMode::VariableOwnThread) {
			update(deltaTime);
		}
		else if (m_updateMode == SubsystemUpdateMode::FixedOwnThread) {
			m_accumulator += deltaTime;
			while (m_accumulator >= m_fixedDeltaTime) {
				fixedUpdate(m_fixedDeltaTime);
				m_accumulator -= m_fixedDeltaTime;
			}
		}

		std::this_thread::yield();
	}
}

void Subsystem::protectedRun(const std::function<void()>& action) {
	bool crashed = false;

#ifdef EE_WINDOWS
	__try {
		action();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		crashed = true;
	}
#else
	// Linux: exceptions are disabled; fatal signals require signal-handler
	// based recovery which is not yet implemented.
	action();
#endif

	if (crashed) {
		setState(SubsystemState::Crashed);
		EError("Subsystem '{}' crashed during update.", m_name);
		if (!tryRecover()) {
			ECrash("Subsystem '%s' crashed and recovery failed.", m_name.c_str());
		}
	}
}

EE_NAMESPACE_END
