#include <Physics/PhysicsBodySubsystem.hpp>
#include <Physics/PhysicsWorldSubsystem.hpp>
#include <Core/Log.hpp>

#ifdef EE_PHYSICS_BACKEND_PHYSX5
# include "PhysX5/PhysXBodyBackend.hpp"
# define BACKEND_CLASS PhysX5::PhysXBodyBackend
#else
# include "Jolt/JoltBodyBackend.hpp"
# define BACKEND_CLASS Jolt::JoltBodyBackend
#endif

EE_NAMESPACE_PHYSICS_BEGIN

struct PhysicsBodySubsystem::Impl {
	BACKEND_CLASS backend;
	PhysicsWorldSubsystem* world = nullptr;
};

PhysicsBodySubsystem::PhysicsBodySubsystem() : Subsystem("PhysicsBody"), m_impl(std::make_unique<Impl>()) {}
PhysicsBodySubsystem::~PhysicsBodySubsystem() = default;

void PhysicsBodySubsystem::attachToWorld(PhysicsWorldSubsystem* world) {
	m_impl->world = world;
	if (world) {
#ifdef EE_PHYSICS_BACKEND_PHYSX5
		m_impl->backend.setPhysicsWorld(world->sdk(), world->scene());
#else
		m_impl->backend.setBodyInterface(world->bodyInterface());
#endif
	}
}

RigidBodyHandle PhysicsBodySubsystem::createRigidBody(const RigidBodyDesc& desc) {
	auto r = m_impl->backend.createBody(desc);
	if (r == InvalidRigidBody) EError("PhysicsBody: create body failed");
	return r;
}

void PhysicsBodySubsystem::destroyRigidBody(RigidBodyHandle h) { m_impl->backend.destroyBody(h); }
ColliderHandle PhysicsBodySubsystem::createCollider(RigidBodyHandle b, const ColliderDesc& d) { return m_impl->backend.createCollider(b, d); }
void PhysicsBodySubsystem::destroyCollider(ColliderHandle h) { m_impl->backend.destroyCollider(h); }
Vec3  PhysicsBodySubsystem::getPosition(RigidBodyHandle h) const { return m_impl->backend.getPosition(h); }
Quat  PhysicsBodySubsystem::getRotation(RigidBodyHandle h) const { return m_impl->backend.getRotation(h); }
Transform PhysicsBodySubsystem::getWorldTransform(RigidBodyHandle h) const { return m_impl->backend.getWorldTransform(h); }
void  PhysicsBodySubsystem::setLinearVelocity(RigidBodyHandle h, const Vec3& v) { m_impl->backend.setLinearVelocity(h, v); }
void  PhysicsBodySubsystem::setPosition(RigidBodyHandle h, const Vec3& pos) { m_impl->backend.setPosition(h, pos); }
void  PhysicsBodySubsystem::addForce(RigidBodyHandle h, const Vec3& f) { m_impl->backend.addForce(h, f); }
Vec3  PhysicsBodySubsystem::getLinearVelocity(RigidBodyHandle h) const { return m_impl->backend.getLinearVelocity(h); }
void  PhysicsBodySubsystem::setAngularLock(RigidBodyHandle h, AngularLockFlag flags) { m_impl->backend.setAngularLock(h, flags); }
void  PhysicsBodySubsystem::setKinematicTarget(RigidBodyHandle h, const Vec3& pos, const Quat& rot) { m_impl->backend.setKinematicTarget(h, pos, rot); }
void  PhysicsBodySubsystem::setKinematic(RigidBodyHandle h, bool k) { m_impl->backend.setKinematic(h, k); }
RigidBodyHandle PhysicsBodySubsystem::createStaticMeshBody(const Vec3* verts, UInt32 vertCount, const UInt32* indices, UInt32 idxCount, const Vec3& pos, const Quat& rot) { return m_impl->backend.createStaticMeshBody(verts, vertCount, indices, idxCount, pos, rot); }
RigidBodyHandle PhysicsBodySubsystem::createConvexBody(const Vec3* verts, UInt32 vertCount, const Vec3& pos, const Quat& rot, F32 mass) { return m_impl->backend.createConvexBody(verts, vertCount, pos, rot, mass); }
RigidBodyHandle PhysicsBodySubsystem::createStaticMeshFromCooked(const void* data, size_t size, const Vec3& pos, const Quat& rot) { return m_impl->backend.createStaticMeshFromCooked(data, size, pos, rot); }
RigidBodyHandle PhysicsBodySubsystem::createConvexFromCooked(const void* data, size_t size, const Vec3& pos, const Quat& rot, F32 mass) { return m_impl->backend.createConvexFromCooked(data, size, pos, rot, mass); }
RigidBodyHandle PhysicsBodySubsystem::createDynamicMeshBody(const Vec3* verts, UInt32 vertCount, const UInt32* indices, UInt32 idxCount, const Vec3& pos, const Quat& rot, F32 mass) { return m_impl->backend.createDynamicMeshBody(verts, vertCount, indices, idxCount, pos, rot, mass); }
RigidBodyHandle PhysicsBodySubsystem::createDynamicMeshFromCooked(const void* data, size_t size, const Vec3& pos, const Quat& rot, F32 mass) { return m_impl->backend.createDynamicMeshFromCooked(data, size, pos, rot, mass); }
UInt32 PhysicsBodySubsystem::bodyCount() const { return m_impl->backend.bodyCount(); }

Result<void, CoreError> PhysicsBodySubsystem::onInitialize() { return {}; }
void PhysicsBodySubsystem::onShutdown() {}

EE_NAMESPACE_PHYSICS_END
