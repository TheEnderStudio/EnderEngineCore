#pragma once

#include "Macros.h"
#include "Types.hpp"
#include "Object.hpp"
#include "Event.hpp"
#include "Errors.hpp"

#include <atomic>
#include <thread>
#include <condition_variable>

EE_NAMESPACE_BEGIN

/**
 * @brief Update mode for a Subsystem.
 */
enum class SubsystemUpdateMode {
	/// @brief Fixed timestep update driven on the main thread.
	FixedMainThread,
	/// @brief Fixed timestep update running on a dedicated subsystem thread.
	FixedOwnThread,
	/// @brief Variable timestep update running on a dedicated subsystem thread.
	VariableOwnThread,
};

/**
 * @brief Runtime state of a Subsystem.
 */
enum class SubsystemState {
	Initialized,
	Running,
	Paused,
	Crashed,
	Shutdown,
};

/**
 * @brief Base class for all engine subsystems.
 *
 * A Subsystem owns the business logic associated with registered Objects.
 * It maintains a bidirectional mapping between Object and an internal handle,
 * exposes an event bus, and supports multiple update/execution modes.
 *
 * Crash recovery: when an exception/crash occurs inside a protected update,
 * onRecover() is invoked. If recovery fails the engine is crashed via ECrash.
 */
class EE_API Subsystem {
public:
	/**
	 * @brief Construct a named subsystem.
	 * @param name Human-readable subsystem name (used for logging).
	 */
	explicit Subsystem(StringView name);

	/**
	 * @brief Destroy the subsystem. Shutdown() is called if still running.
	 */
	virtual ~Subsystem();

	EE_NO_COPY(Subsystem)
	EE_NO_MOVE(Subsystem)

	/**
	 * @brief Initialize the subsystem.
	 * @return Result indicating success or failure.
	 */
	Result<void, CoreError> initialize();

	/**
	 * @brief Shut down the subsystem and release resources.
	 */
	void shutdown();

	/**
	 * @brief Register an Object with this subsystem.
	 * @param object The object to register.
	 * @return Result indicating success or failure.
	 */
	Result<void, CoreError> registerObject(const Object& object);

	/**
	 * @brief Unregister an Object from this subsystem.
	 * @param object The object to unregister.
	 */
	void unregisterObject(const Object& object);

	/**
	 * @brief Check whether an Object is registered.
	 * @param object The object to query.
	 * @return true if registered.
	 */
	EE_NODISCARD bool hasObject(const Object& object) const;

	/**
	 * @brief Subscribe an Object to events of type E emitted by this subsystem.
	 * @tparam E Concrete event type derived from Event.
	 * @tparam Handler Callable with signature void(const E&).
	 * @param object The object that listens to the event.
	 * @param handler Callback invoked when the event is emitted.
	 * @return A subscription handle, or InvalidSubscriptionId if the object is not registered.
	 */
	template <typename E, typename Handler>
		requires std::is_base_of_v<Event, E>
	SubscriptionId subscribe(const Object& object, Handler&& handler) {
		std::lock_guard<Mutex> lock(m_objectMutex);
		if (!m_objectToInternal.contains(object.guid())) {
			return InvalidSubscriptionId;
		}
		SubscriptionId id = m_eventBus.subscribe<E>(std::forward<Handler>(handler));
		m_objectSubscriptions[object.guid()].push_back(id);
		return id;
	}

	/**
	 * @brief Emit an event to all subscribers of its type.
	 * @tparam E Concrete event type derived from Event.
	 * @param event The event to emit.
	 */
	template <typename E>
		requires std::is_base_of_v<Event, E>
	void emit(const E& event) {
		m_eventBus.publish(event);
	}

	/**
	 * @brief Perform a single variable-step update.
	 *
	 * Should be called by the engine loop for FixedMainThread mode or
	 * internally by the subsystem thread for VariableOwnThread mode.
	 * @param deltaTime Time in seconds since the last update.
	 */
	void update(F64 deltaTime);

	/**
	 * @brief Perform a single fixed-step update.
	 *
	 * Should be called by the engine loop for FixedMainThread mode or
	 * internally by the subsystem thread for FixedOwnThread mode.
	 * @param fixedDeltaTime Fixed timestep in seconds.
	 */
	void fixedUpdate(F64 fixedDeltaTime);

	/**
	 * @brief Start the subsystem's dedicated update thread.
	 *
	 * Only valid for FixedOwnThread or VariableOwnThread modes.
	 * @return Result indicating success or failure.
	 */
	Result<void, CoreError> startThread();

	/**
	 * @brief Stop the subsystem's dedicated update thread.
	 */
	void stopThread();

	/// @brief Pause updates without shutting down.
	void pause();

	/// @brief Resume updates after a pause.
	void resume();

	/**
	 * @brief Attempt to recover from a crash.
	 * @return true if recovery succeeded.
	 */
	bool tryRecover();

	/// @return The subsystem name.
	EE_NODISCARD const String& name() const { return m_name; }

	/// @return The current subsystem state.
	EE_NODISCARD SubsystemState state() const { return m_state.load(std::memory_order_acquire); }

	/// @return The configured update mode.
	EE_NODISCARD SubsystemUpdateMode updateMode() const { return m_updateMode; }

	/**
	 * @brief Get the internal handle associated with an Object.
	 * @param object The registered object.
	 * @return The internal handle, or 0 if not registered.
	 */
	EE_NODISCARD UInt32 getInternalHandle(const Object& object) const;

	/**
	 * @brief Get the Object associated with an internal handle.
	 * @param handle The internal handle.
	 * @return The Object, or NullOpt if the handle is invalid.
	 */
	EE_NODISCARD Optional<Object> getObject(UInt32 handle) const;

protected:
	/**
	 * @brief Set the update mode.
	 *
	 * Must be called from the derived constructor before initialize().
	 * @param mode The desired update mode.
	 */
	void setUpdateMode(SubsystemUpdateMode mode) { m_updateMode = mode; }

	/**
	 * @brief Set the fixed update rate.
	 * @param rate Updates per second.
	 */
	void setFixedUpdateRate(F64 rate) { m_fixedDeltaTime = 1.0 / rate; }

	/// @brief Called once during initialize().
	virtual Result<void, CoreError> onInitialize() { return {}; }

	/// @brief Called once during shutdown().
	virtual void onShutdown() {}

	/// @brief Called for variable-step updates.
	virtual void onUpdate(F64 deltaTime) { EE_UNUSED(deltaTime); }

	/// @brief Called for fixed-step updates.
	virtual void onFixedUpdate(F64 fixedDeltaTime) { EE_UNUSED(fixedDeltaTime); }

	/**
	 * @brief Called after a new Object has been registered and mapped.
	 * @param object The registered object.
	 * @param handle The internal handle assigned to the object.
	 */
	virtual void onObjectRegistered(const Object& object, UInt32 handle) {
		EE_UNUSED(object);
		EE_UNUSED(handle);
	}

	/**
	 * @brief Called before an Object is unregistered.
	 * @param object The object being unregistered.
	 * @param handle The internal handle that was assigned to the object.
	 */
	virtual void onObjectUnregistered(const Object& object, UInt32 handle) {
		EE_UNUSED(object);
		EE_UNUSED(handle);
	}

	/**
	 * @brief Called when a protected update crashes and recovery is attempted.
	 *
	 * Derived classes may reset internal state to a safe default. Returning
	 * false will cause the engine to crash.
	 * @return true if the subsystem recovered successfully.
	 */
	virtual bool onRecover() { return false; }

private:
	using InternalHandle = UInt32;

	void threadLoop();
	void protectedRun(const std::function<void()>& action);
	void setState(SubsystemState state) { m_state.store(state, std::memory_order_release); }

	String m_name;
	SubsystemUpdateMode m_updateMode = SubsystemUpdateMode::FixedMainThread;
	std::atomic<SubsystemState> m_state{ SubsystemState::Initialized };

	HashMap<Guid, InternalHandle> m_objectToInternal;
	HashMap<InternalHandle, Object> m_internalToObject;
	HashMap<Guid, Vector<SubscriptionId>> m_objectSubscriptions;
	InternalHandle m_nextInternalHandle = 1;
	mutable Mutex m_objectMutex;

	EventBus m_eventBus;

	F64 m_fixedDeltaTime = 1.0 / 60.0;
	F64 m_accumulator = 0.0;

	std::thread m_updateThread;
	std::atomic<bool> m_threadRunning{false};
	std::atomic<bool> m_paused{false};
	std::condition_variable m_pauseCondition;
	mutable Mutex m_pauseMutex;
};

EE_NAMESPACE_END
