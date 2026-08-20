#pragma once

#include <Core/Subsystem.hpp>
#include "Errors.hpp"
#include "PhysicsTypes.hpp"
#include "PhysicsEvents.hpp"

EE_NAMESPACE_PHYSICS_BEGIN

/// @brief Subsystem managing the physics world, simulation stepping, and scene queries.
class EE_API PhysicsWorldSubsystem : public Subsystem {
public:
	/// @brief Default constructor.
	PhysicsWorldSubsystem();
	/// @brief Destructor.
	~PhysicsWorldSubsystem() override;

	EE_NO_COPY(PhysicsWorldSubsystem)
	EE_NO_MOVE(PhysicsWorldSubsystem)

	/// @brief Initialize the physics world with the given descriptor.
	Result<void, PhysicsError> initialize(const PhysicsWorldDesc& desc);

	/// @brief Start the physics simulation.
	void start();
	/// @brief Stop the physics simulation.
	void stop();
	/// @brief Check whether the physics simulation is currently running.
	bool isRunning() const;
	/// @brief Advance the simulation by a time step.
	Result<void, PhysicsError> step(F32 dt);

	/// @brief Set the world gravity vector.
	void setGravity(const Vec3& g);
	/// @brief Get the current world gravity vector.
	Vec3 gravity() const;

	/// @brief Cast a ray into the scene.
	RaycastHit raycast(const Vec3& origin, const Vec3& direction, F32 maxDistance = 1000.0f, const RaycastFilter& filter = {}) const;

	/// @brief Find all bodies overlapping a box.
	OverlapResult overlapBox(const Vec3& center, const Vec3& halfExtents) const;

	/// @brief Find all bodies overlapping a sphere.
	OverlapResult overlapSphere(const Vec3& center, F32 radius) const;

#ifdef EE_PHYSICS_BACKEND_PHYSX5
	/// @brief Get the PhysX SDK pointer (PhysX5 backend only).
	void* sdk() const;
	/// @brief Get the PhysX scene pointer (PhysX5 backend only).
	void* scene() const;
#else
	/// @brief Get the Jolt body interface pointer (Jolt backend only).
	void* bodyInterface() const;
#endif
	/// @brief Retrieve contact pairs after simulation step (PhysX5 backend only).
	Vector<ContactPair> fetchContacts();

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;
	void onUpdate(F64 deltaTime) override;
	bool onRecover() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_PHYSICS_END
