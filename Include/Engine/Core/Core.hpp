#pragma once

#include "Macros.h"
#include "Types.hpp"
#include "Errors.hpp"
#include "Version.hpp"

EE_NAMESPACE_BEGIN

class Subsystem;

/**
 * @brief Engine global state management.
 *
 * Provides the main entry point for initializing and shutting down the engine core,
 * as well as registering and updating subsystems.
 */
class EE_API Engine {
public:
	/**
	 * @brief Initialize the engine core (logging, crash handling, etc.).
	 * @return Result indicating success or failure.
	 */
	static Result<void, CoreError> initialize();

	/**
	 * @brief Shut down the engine core and all registered subsystems.
	 *
	 * This function is idempotent; multiple calls are safe.
	 */
	static void shutdown();

	/**
	 * @brief Check whether the engine has been initialized.
	 * @return true if initialized.
	 */
	static bool isInitialized();

	/**
	 * @brief Register a subsystem with the engine.
	 * @param subsystem The subsystem to register.
	 * @return Result indicating success or failure.
	 */
	static Result<void, CoreError> registerSubsystem(Subsystem* subsystem);

	/**
	 * @brief Unregister a subsystem from the engine.
	 * @param subsystem The subsystem to unregister.
	 */
	static void unregisterSubsystem(Subsystem* subsystem);

	/**
	 * @brief Get all registered subsystems.
	 * @return Const reference to the list of registered subsystems.
	 */
	static const Vector<Subsystem*>& subsystems();

	/**
	 * @brief Update all FixedMainThread subsystems with variable timestep.
	 * @param deltaTime Time in seconds since the last update.
	 */
	static void update(F64 deltaTime);

	/**
	 * @brief Fixed-update all FixedMainThread subsystems.
	 * @param fixedDeltaTime Fixed timestep in seconds.
	 */
	static void fixedUpdate(F64 fixedDeltaTime);

	static const Version& getVersion();
};

EE_NAMESPACE_END
