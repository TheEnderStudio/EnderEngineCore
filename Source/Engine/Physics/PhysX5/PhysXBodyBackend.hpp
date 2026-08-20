#pragma once

#include <Core/Types.hpp>
#include <Physics/PhysicsTypes.hpp>
#include <Physics/Errors.hpp>

EE_NAMESPACE_PHYSICS_BEGIN
namespace PhysX5 {

class PhysXBodyBackend {
public:
	PhysXBodyBackend();
	~PhysXBodyBackend();

	EE_NO_COPY(PhysXBodyBackend)

	void setPhysicsWorld(void* sdk, void* scene);

	RigidBodyHandle createBody(const RigidBodyDesc& desc);
	void destroyBody(RigidBodyHandle handle);
	ColliderHandle createCollider(RigidBodyHandle body, const ColliderDesc& desc);
	void destroyCollider(ColliderHandle handle);

	/// Create a static body with triangle mesh collision from vertex/index data.
	RigidBodyHandle createStaticMeshBody(const Vec3* verts, UInt32 vertCount,
	                                     const UInt32* indices, UInt32 idxCount,
	                                     const Vec3& pos, const Quat& rot);
	/// Create a dynamic body with convex mesh collision from vertex data.
	RigidBodyHandle createConvexBody(const Vec3* verts, UInt32 vertCount,
	                                 const Vec3& pos, const Quat& rot, F32 mass);

	/// Load pre-cooked triangle mesh from binary data.
	RigidBodyHandle createStaticMeshFromCooked(const void* data, size_t size,
	                                            const Vec3& pos, const Quat& rot);

	/// Load pre-cooked convex mesh from binary data.
	RigidBodyHandle createConvexFromCooked(const void* data, size_t size,
	                                        const Vec3& pos, const Quat& rot, F32 mass);

	/// Create a dynamic body with triangle mesh collision (mass must be provided).
	RigidBodyHandle createDynamicMeshBody(const Vec3* verts, UInt32 vertCount,
	                                       const UInt32* indices, UInt32 idxCount,
	                                       const Vec3& pos, const Quat& rot, F32 mass);

	/// @brief Load pre-cooked triangle mesh and create a dynamic body.
	RigidBodyHandle createDynamicMeshFromCooked(const void* data, size_t size,
	                                             const Vec3& pos, const Quat& rot, F32 mass);

	Vec3  getPosition(RigidBodyHandle handle) const;
	Quat  getRotation(RigidBodyHandle handle) const;
	Transform getWorldTransform(RigidBodyHandle handle) const;
	void  setLinearVelocity(RigidBodyHandle handle, const Vec3& v);
	Vec3  getLinearVelocity(RigidBodyHandle handle) const;
	void  addForce(RigidBodyHandle handle, const Vec3& f);
	void  setKinematicTarget(RigidBodyHandle handle, const Vec3& pos, const Quat& rot);
	void  setPosition(RigidBodyHandle handle, const Vec3& pos);
	void setAngularLock(RigidBodyHandle handle, AngularLockFlag flags);
	void setKinematic(RigidBodyHandle handle, bool kinematic);

	UInt32 bodyCount() const;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

} // namespace PhysX5
EE_NAMESPACE_PHYSICS_END
