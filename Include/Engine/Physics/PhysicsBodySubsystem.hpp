#pragma once

#include <Core/Subsystem.hpp>
#include "Errors.hpp"
#include "PhysicsTypes.hpp"

EE_NAMESPACE_PHYSICS_BEGIN

/// @brief Subsystem managing rigid bodies and colliders.
class EE_API PhysicsBodySubsystem : public Subsystem {
public:
	/// @brief Default constructor.
	PhysicsBodySubsystem();
	/// @brief Destructor.
	~PhysicsBodySubsystem() override;

	EE_NO_COPY(PhysicsBodySubsystem)
	EE_NO_MOVE(PhysicsBodySubsystem)

	/// @brief Attach this body subsystem to a physics world.
	void attachToWorld(class PhysicsWorldSubsystem* world);

	/// @brief Create a rigid body from a descriptor.
	RigidBodyHandle createRigidBody(const RigidBodyDesc& desc);
	/// @brief Destroy a rigid body by handle.
	void destroyRigidBody(RigidBodyHandle handle);
	/// @brief Create a collider and attach it to a rigid body.
	ColliderHandle createCollider(RigidBodyHandle body, const ColliderDesc& desc);
	/// @brief Destroy a collider by handle.
	void destroyCollider(ColliderHandle handle);

	/// @brief Get the world-space position of a rigid body.
	Vec3 getPosition(RigidBodyHandle handle) const;
	/// @brief Get the world-space rotation of a rigid body.
	Quat getRotation(RigidBodyHandle handle) const;
	/// @brief Get the world transform of a rigid body.
	Transform getWorldTransform(RigidBodyHandle handle) const;
	/// @brief Set the linear velocity of a rigid body.
	void setLinearVelocity(RigidBodyHandle handle, const Vec3& v);
	/// @brief Set the world-space position of a rigid body.
	void setPosition(RigidBodyHandle handle, const Vec3& pos);
	/// @brief Apply a force to a rigid body.
	void addForce(RigidBodyHandle handle, const Vec3& force);
	/// @brief Get the linear velocity of a rigid body.
	Vec3 getLinearVelocity(RigidBodyHandle handle) const;
	/// @brief Lock angular rotation around specified axes.
	void setAngularLock(RigidBodyHandle handle, AngularLockFlag flags);
	/// @brief Set the target position and rotation for a kinematic body.
	void setKinematicTarget(RigidBodyHandle handle, const Vec3& pos, const Quat& rot = Quat(1,0,0,0));
	/// @brief Set whether the body is kinematic.
	void setKinematic(RigidBodyHandle handle, bool kinematic);

	/// @brief Create a static body with triangle mesh from vertex/index data.
	EE_DEPRECATED("Runtime-cooking has a significant impact on performance. Cook the mesh with tool 'MeshCooking' first and then use ***FromCooked instead.")
		RigidBodyHandle createStaticMeshBody(const Vec3* verts, UInt32 vertCount, const UInt32* indices, UInt32 idxCount, const Vec3& pos, const Quat& rot = Quat(1, 0, 0, 0));

	/// @brief Create a dynamic body with convex hull from vertex data.
	EE_DEPRECATED("Runtime-cooking has a significant impact on performance. Cook the mesh with tool 'MeshCooking' first and then use ***FromCooked instead.")
		RigidBodyHandle createConvexBody(const Vec3* verts, UInt32 vertCount, const Vec3& pos, const Quat& rot = Quat(1,0,0,0), F32 mass = 1.0f);

	/// @brief Load pre-cooked triangle mesh from binary data (from MeshCooking tool).
	RigidBodyHandle createStaticMeshFromCooked(const void* data, size_t size, const Vec3& pos, const Quat& rot = Quat(1,0,0,0));

	/// @brief Load pre-cooked convex mesh from binary data.
	RigidBodyHandle createConvexFromCooked(const void* data, size_t size, const Vec3& pos, const Quat& rot = Quat(1,0,0,0), F32 mass = 1.0f);

	/// @brief Create a dynamic body with triangle mesh collider from vertex/index data (mass must be provided).
	EE_DEPRECATED("Runtime-cooking has a significant impact on performance. Cook the mesh with tool 'MeshCooking' first and then use ***FromCooked instead.")
		RigidBodyHandle createDynamicMeshBody(const Vec3* verts, UInt32 vertCount, const UInt32* indices, UInt32 idxCount, const Vec3& pos, const Quat& rot = Quat(1,0,0,0), F32 mass = 1.0f);

	/// @brief Load pre-cooked triangle mesh and create a dynamic body.
	RigidBodyHandle createDynamicMeshFromCooked(const void* data, size_t size, const Vec3& pos, const Quat& rot = Quat(1,0,0,0), F32 mass = 1.0f);

	/// @brief Get the number of active bodies.
	UInt32 bodyCount() const;

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_PHYSICS_END
