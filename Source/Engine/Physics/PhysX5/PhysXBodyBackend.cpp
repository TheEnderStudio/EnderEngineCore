#include "PhysXBodyBackend.hpp"
#include <Core/Log.hpp>

#ifdef EE_PHYSICS_BACKEND_PHYSX5

#include <PxPhysicsAPI.h>
#include <extensions/PxRigidBodyExt.h>
#include <extensions/PxDefaultStreams.h>
#include <cooking/PxCooking.h>
#include <mutex>
#include <unordered_map>

EE_NAMESPACE_PHYSICS_BEGIN
namespace PhysX5 {

using namespace physx;

struct RigidActorEntry {
	PxRigidActor* actor = nullptr;
	RigidBodyDesc desc;
	UInt64        id;
};

struct ColliderEntry {
	PxShape*          shape = nullptr;
	ColliderDesc      desc;
	RigidBodyHandle   parentBody = InvalidRigidBody;
	UInt64            id;
};

struct PhysXBodyBackend::Impl {
	PxPhysics* sdk     = nullptr;
	PxScene*   scene   = nullptr;

	UInt64 nextBodyId     = 1;
	UInt64 nextColliderId = 1;

	std::unordered_map<RigidBodyHandle, RigidActorEntry> bodies;
	std::unordered_map<ColliderHandle, ColliderEntry>    colliders;
	mutable std::mutex mutex;
};

// ---------- helpers ----------

static PxGeometryHolder toGeometry(const ColliderDesc& desc) {
	PxGeometryHolder g;
	switch (desc.shape) {
	case ColliderShape::Capsule:
		g = PxCapsuleGeometry(desc.radius, desc.height * 0.5f);
		break;
	case ColliderShape::Box:
		g = PxBoxGeometry(desc.halfExtents.x, desc.halfExtents.y, desc.halfExtents.z);
		break;
	case ColliderShape::Sphere:
		g = PxSphereGeometry(desc.halfExtents.x);
		break;
	}
	return g;
}

static PxTransform toPose(const Vec3& pos, const Quat& rot) {
	return PxTransform(PxVec3(pos.x, pos.y, pos.z),
	                   PxQuat(rot.x, rot.y, rot.z, rot.w));
}

// ---------- backend ----------

PhysXBodyBackend::PhysXBodyBackend() : m_impl(std::make_unique<Impl>()) {}
PhysXBodyBackend::~PhysXBodyBackend() = default;

void PhysXBodyBackend::setPhysicsWorld(void* sdk, void* scene) {
	auto& p = *m_impl;
	p.sdk   = static_cast<PxPhysics*>(sdk);
	p.scene = static_cast<PxScene*>(scene);
}

RigidBodyHandle PhysXBodyBackend::createBody(const RigidBodyDesc& desc) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxRigidActor* actor = nullptr;
	if (desc.type == RigidBodyType::Static) {
		actor = p.sdk->createRigidStatic(toPose(desc.position, desc.rotation));
	} else {
		PxRigidDynamic* dyn = p.sdk->createRigidDynamic(toPose(desc.position, desc.rotation));
		dyn->setLinearVelocity(PxVec3(desc.linearVelocity.x, desc.linearVelocity.y, desc.linearVelocity.z));
		dyn->setAngularVelocity(PxVec3(0));
		if (desc.type == RigidBodyType::Kinematic)
			dyn->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
		actor = dyn;
	}

	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, desc, h };
	return h;
}

void PhysXBodyBackend::destroyBody(RigidBodyHandle handle) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end()) return;
	if (it->second.actor) {
		p.scene->removeActor(*it->second.actor);
		it->second.actor->release();
	}
	p.bodies.erase(it);
}

ColliderHandle PhysXBodyBackend::createCollider(RigidBodyHandle bodyHandle, const ColliderDesc& desc) {
	auto& p = *m_impl;
	if (!p.sdk) return InvalidCollider;
	std::lock_guard<std::mutex> lk(p.mutex);

	auto bit = p.bodies.find(bodyHandle);
	if (bit == p.bodies.end()) return InvalidCollider;

	// Save old state
	PxRigidActor* oldActor = bit->second.actor;
	PxTransform oldPose = oldActor->getGlobalPose();
	PxVec3 oldVel(0);
	PxRigidDynamic* oldDyn = oldActor->is<PxRigidDynamic>();
	if (oldDyn) oldVel = oldDyn->getLinearVelocity();

	// Build new shape
	PxGeometryHolder geom = toGeometry(desc);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom.any(), *mat, true);
	shape->setLocalPose(PxTransform(
		PxVec3(desc.localPos.x, desc.localPos.y, desc.localPos.z),
		PxQuat(desc.localRot.x, desc.localRot.y, desc.localRot.z, desc.localRot.w)));

	// Remove old, create new with shape
	p.scene->removeActor(*oldActor);
	oldActor->release();

	PxRigidActor* newActor = nullptr;
	if (bit->second.desc.type == RigidBodyType::Static) {
		newActor = p.sdk->createRigidStatic(oldPose);
	} else {
		PxRigidDynamic* dyn = p.sdk->createRigidDynamic(oldPose);
		dyn->setLinearVelocity(oldVel);
		if (bit->second.desc.type == RigidBodyType::Kinematic)
			dyn->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
		newActor = dyn;
	}
	newActor->attachShape(*shape);
	p.scene->addActor(*newActor);
	newActor->userData = (void*)(uintptr_t)bodyHandle;

	// Mass properties
	PxRigidBody* rb = newActor->is<PxRigidBody>();
	if (rb) PxRigidBodyExt::updateMassAndInertia(*rb, bit->second.desc.mass);

	bit->second.actor = newActor;

	UInt64 h = p.nextColliderId++;
	p.colliders[h] = { shape, desc, bodyHandle, h };
	return h;
}

void PhysXBodyBackend::destroyCollider(ColliderHandle handle) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.colliders.find(handle);
	if (it == p.colliders.end()) return;
	if (it->second.shape) it->second.shape->release();
	p.colliders.erase(it);
}

Vec3 PhysXBodyBackend::getPosition(RigidBodyHandle handle) const {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return Vec3(0);
	PxVec3 pos = it->second.actor->getGlobalPose().p;
	return Vec3(pos.x, pos.y, pos.z);
}

Quat PhysXBodyBackend::getRotation(RigidBodyHandle handle) const {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return Quat(1,0,0,0);
	PxQuat q = it->second.actor->getGlobalPose().q;
	return Quat(q.w, q.x, q.y, q.z);
}

Transform PhysXBodyBackend::getWorldTransform(RigidBodyHandle handle) const {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return Transform{};
	PxTransform pose = it->second.actor->getGlobalPose();

	//PxMat44 m(pose);
	//Mat4 world;
	//world[0] = Vec4(m.column0.x, m.column0.y, m.column0.z, m.column0.w);
	//world[1] = Vec4(m.column1.x, m.column1.y, m.column1.z, m.column1.w);
	//world[2] = Vec4(m.column2.x, m.column2.y, m.column2.z, m.column2.w);
	//world[3] = Vec4(m.column3.x, m.column3.y, m.column3.z, m.column3.w);

	PxMat44 pxmat(pose);

	Transform t;
	t.position.x = pose.p.x;
	t.position.y = pose.p.y;
	t.position.z = pose.p.z;
	t.rotation.x = pose.q.x;
	t.rotation.y = pose.q.y;
	t.rotation.z = pose.q.z;
	t.rotation.w = pose.q.w;

	Mat4 eemat = t.computeWorldMatrix();

	return t;
}

void PhysXBodyBackend::setLinearVelocity(RigidBodyHandle handle, const Vec3& v) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return;
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (dyn) dyn->setLinearVelocity(PxVec3(v.x, v.y, v.z));
}

Vec3 PhysXBodyBackend::getLinearVelocity(RigidBodyHandle handle) const {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return Vec3(0);
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (!dyn) return Vec3(0);
	PxVec3 v = dyn->getLinearVelocity();
	return Vec3(v.x, v.y, v.z);
}

void PhysXBodyBackend::addForce(RigidBodyHandle handle, const Vec3& f) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return;
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (dyn) dyn->addForce(PxVec3(f.x, f.y, f.z));
}

void PhysXBodyBackend::setKinematicTarget(RigidBodyHandle handle, const Vec3& pos, const Quat& rot) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return;
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (dyn) dyn->setKinematicTarget(toPose(pos, rot));
}

void PhysXBodyBackend::setKinematic(RigidBodyHandle handle, bool kinematic) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return;
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (dyn) dyn->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, kinematic);
}

void PhysXBodyBackend::setPosition(RigidBodyHandle handle, const Vec3& pos) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return;
	it->second.actor->setGlobalPose(PxTransform(PxVec3(pos.x, pos.y, pos.z),
	                                            it->second.actor->getGlobalPose().q));
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (dyn) dyn->wakeUp();
}

void PhysXBodyBackend::setAngularLock(RigidBodyHandle handle, AngularLockFlag flags) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	auto it = p.bodies.find(handle);
	if (it == p.bodies.end() || !it->second.actor) return;
	PxRigidDynamic* dyn = it->second.actor->is<PxRigidDynamic>();
	if (!dyn) return;
	PxRigidDynamicLockFlags lockFlags;
	if (flags & AngularLockFlag::LockX) lockFlags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
	if (flags & AngularLockFlag::LockY) lockFlags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
	if (flags & AngularLockFlag::LockZ) lockFlags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
	dyn->setRigidDynamicLockFlags(lockFlags);
}

RigidBodyHandle PhysXBodyBackend::createStaticMeshBody(const Vec3* verts, UInt32 vertCount,
                                                       const UInt32* indices, UInt32 idxCount,
                                                       const Vec3& pos, const Quat& rot) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxTolerancesScale scale;
	PxCookingParams cookParams(scale);

	PxTriangleMeshDesc meshDesc;
	meshDesc.points.count     = vertCount;
	meshDesc.points.stride    = sizeof(Vec3);
	meshDesc.points.data      = verts;
	meshDesc.triangles.count  = idxCount / 3;
	meshDesc.triangles.stride = sizeof(UInt32) * 3;
	meshDesc.triangles.data   = indices;

	PxTriangleMesh* triMesh = PxCreateTriangleMesh(cookParams, meshDesc, p.sdk->getPhysicsInsertionCallback());
	if (!triMesh) { EError("Physics: createTriangleMesh failed"); return InvalidRigidBody; }

	PxTriangleMeshGeometry geom(triMesh);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom, *mat, true);

	PxTransform pose(PxVec3(pos.x, pos.y, pos.z), PxQuat(rot.x, rot.y, rot.z, rot.w));
	PxRigidStatic* actor = p.sdk->createRigidStatic(pose);
	actor->attachShape(*shape);
	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, RigidBodyDesc{}, h };
	return h;
}

RigidBodyHandle PhysXBodyBackend::createConvexBody(const Vec3* verts, UInt32 vertCount,
                                                   const Vec3& pos, const Quat& rot, F32 mass) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxTolerancesScale scale;
	PxCookingParams cookParams(scale);

	// Limit vertices for convex hull (PhysX recommends ≤256)
	Vector<Vec3> subset;
	const Vec3* useVerts = verts;
	UInt32 useCount = vertCount;
	if (vertCount > 256) {
		// Uniform subsample
		UInt32 step = vertCount / 256 + 1;
		for (UInt32 i = 0; i < vertCount; i += step)
			subset.push_back(verts[i]);
		useVerts = subset.data();
		useCount = (UInt32)subset.size();
		EInfo("Physics: convex hull reduced from {} to {} vertices", vertCount, useCount);
	}

	PxConvexMeshDesc convexDesc;
	convexDesc.points.count  = useCount;
	convexDesc.points.stride = sizeof(Vec3);
	convexDesc.points.data   = useVerts;
	convexDesc.flags         = PxConvexFlag::eCOMPUTE_CONVEX;

	PxConvexMesh* convexMesh = PxCreateConvexMesh(cookParams, convexDesc, p.sdk->getPhysicsInsertionCallback());
	if (!convexMesh) {
		EError("Physics: createConvexMesh failed ({} vertices, try simplifying mesh)", useCount);
		return InvalidRigidBody;
	}

	PxConvexMeshGeometry geom(convexMesh);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom, *mat, true);

	PxTransform pose(PxVec3(pos.x, pos.y, pos.z), PxQuat(rot.x, rot.y, rot.z, rot.w));
	PxRigidDynamic* actor = p.sdk->createRigidDynamic(pose);
	actor->attachShape(*shape);
	PxRigidBodyExt::updateMassAndInertia(*actor, mass);
	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, RigidBodyDesc{}, h };
	return h;
}

RigidBodyHandle PhysXBodyBackend::createStaticMeshFromCooked(const void* data, size_t size,
                                                              const Vec3& pos, const Quat& rot) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxDefaultMemoryInputData input((PxU8*)data, (PxU32)size);
	PxTriangleMesh* triMesh = p.sdk->createTriangleMesh(input);
	if (!triMesh) { EError("Physics: createTriangleMesh from cooked data failed"); return InvalidRigidBody; }

	PxTriangleMeshGeometry geom(triMesh);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom, *mat, true);

	PxTransform pose(PxVec3(pos.x, pos.y, pos.z), PxQuat(rot.x, rot.y, rot.z, rot.w));
	PxRigidStatic* actor = p.sdk->createRigidStatic(pose);
	actor->attachShape(*shape);
	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, RigidBodyDesc{}, h };
	return h;
}

RigidBodyHandle PhysXBodyBackend::createConvexFromCooked(const void* data, size_t size,
                                                          const Vec3& pos, const Quat& rot, F32 mass) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxDefaultMemoryInputData input((PxU8*)data, (PxU32)size);
	PxConvexMesh* convexMesh = p.sdk->createConvexMesh(input);
	if (!convexMesh) { EError("Physics: createConvexMesh from cooked data failed"); return InvalidRigidBody; }

	PxConvexMeshGeometry geom(convexMesh);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom, *mat, true);

	PxTransform pose(PxVec3(pos.x, pos.y, pos.z), PxQuat(rot.x, rot.y, rot.z, rot.w));
	PxRigidDynamic* actor = p.sdk->createRigidDynamic(pose);
	actor->attachShape(*shape);
	PxRigidBodyExt::updateMassAndInertia(*actor, mass);
	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, RigidBodyDesc{}, h };
	return h;
}

RigidBodyHandle PhysXBodyBackend::createDynamicMeshBody(const Vec3* verts, UInt32 vertCount,
                                                         const UInt32* indices, UInt32 idxCount,
                                                         const Vec3& pos, const Quat& rot, F32 mass) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxTolerancesScale scale;
	PxCookingParams cookParams(scale);

	PxTriangleMeshDesc meshDesc;
	meshDesc.points.count     = vertCount;
	meshDesc.points.stride    = sizeof(Vec3);
	meshDesc.points.data      = verts;
	meshDesc.triangles.count  = idxCount / 3;
	meshDesc.triangles.stride = sizeof(UInt32) * 3;
	meshDesc.triangles.data   = indices;

	PxTriangleMesh* triMesh = PxCreateTriangleMesh(cookParams, meshDesc, p.sdk->getPhysicsInsertionCallback());
	if (!triMesh) { EError("Physics: createTriangleMesh failed"); return InvalidRigidBody; }

	PxTriangleMeshGeometry geom(triMesh);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom, *mat, true);

	PxTransform pose(PxVec3(pos.x, pos.y, pos.z), PxQuat(rot.x, rot.y, rot.z, rot.w));
	PxRigidDynamic* actor = p.sdk->createRigidDynamic(pose);
	actor->attachShape(*shape);
	// SDF-cooked meshes support non-kinematic dynamic bodies
	PxRigidBodyExt::setMassAndUpdateInertia(*actor, mass);
	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, RigidBodyDesc{}, h };
	return h;
}

RigidBodyHandle PhysXBodyBackend::createDynamicMeshFromCooked(const void* data, size_t size,
                                                               const Vec3& pos, const Quat& rot, F32 mass) {
	auto& p = *m_impl;
	if (!p.sdk || !p.scene) return InvalidRigidBody;

	PxDefaultMemoryInputData input((PxU8*)data, (PxU32)size);
	PxTriangleMesh* triMesh = p.sdk->createTriangleMesh(input);
	if (!triMesh) { EError("Physics: createTriangleMesh from cooked data failed"); return InvalidRigidBody; }

	PxTriangleMeshGeometry geom(triMesh);
	PxMaterial* mat = p.sdk->createMaterial(0.5f, 0.5f, 0.3f);
	PxShape* shape = p.sdk->createShape(geom, *mat, true);

	PxTransform pose(PxVec3(pos.x, pos.y, pos.z), PxQuat(rot.x, rot.y, rot.z, rot.w));
	PxRigidDynamic* actor = p.sdk->createRigidDynamic(pose);
	actor->attachShape(*shape);
	// SDF-cooked meshes support non-kinematic dynamic bodies
	PxRigidBodyExt::setMassAndUpdateInertia(*actor, mass);
	p.scene->addActor(*actor);

	UInt64 h = p.nextBodyId++;
	actor->userData = (void*)(uintptr_t)h;
	std::lock_guard<std::mutex> lk(p.mutex);
	p.bodies[h] = { actor, RigidBodyDesc{}, h };
	return h;
}

UInt32 PhysXBodyBackend::bodyCount() const {
	std::lock_guard<std::mutex> lk(m_impl->mutex);
	return static_cast<UInt32>(m_impl->bodies.size());
}

} // namespace PhysX5
EE_NAMESPACE_PHYSICS_END

#endif // EE_PHYSICS_BACKEND_PHYSX5
