#include "PhysXWorldBackend.hpp"
#include <Core/Log.hpp>

#ifdef EE_PHYSICS_BACKEND_PHYSX5

#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultSimulationFilterShader.h>
#include <extensions/PxSimpleFactory.h>
#include <pvd/PxPvd.h>
#include <pvd/PxPvdTransport.h>

EE_NAMESPACE_PHYSICS_BEGIN
namespace PhysX5 {

// ===================================================================
// Custom error callback — routes PhysX errors to engine log
// ===================================================================
class EngineErrorCallback : public physx::PxErrorCallback {
public:
	void reportError(physx::PxErrorCode::Enum code, const char* message, const char* file, int line) override {
		switch (code) {
		case physx::PxErrorCode::eDEBUG_INFO:   Log::debug(file, line, "???", "[PhysX] {}", message); break;
		case physx::PxErrorCode::eDEBUG_WARNING:Log::warn(file, line, "???", "[PhysX] [DEBUG ONLY] {}", message); break;
		case physx::PxErrorCode::ePERF_WARNING: Log::warn(file, line, "???", "[PhysX] [PERF WARN] {}", message); break;
		case physx::PxErrorCode::eINVALID_PARAMETER:
		case physx::PxErrorCode::eINVALID_OPERATION:
		case physx::PxErrorCode::eINTERNAL_ERROR: Log::error(file, line, "???", "[PhysX] {}", message); break;
		case physx::PxErrorCode::eOUT_OF_MEMORY: Log::critical(file, line, "???", "[PhysX] [OUT OF MEM] {}", message); break;
		default: Log::warn(file, line, "???", "[PhysX] {}", message); break;
		}
	}
};

// ===================================================================
// Simulation callback — collects contact pairs for engine events
// ===================================================================
using namespace physx;

class SimulationEventCallback : public PxSimulationEventCallback {
public:
	Vector<ContactPair> contacts;
	mutable std::mutex mutex;

	void onContact(const PxContactPairHeader& pairHeader, const PxContactPair* pairs, PxU32 nbPairs) override {
		std::lock_guard<std::mutex> lk(mutex);
		for (PxU32 i = 0; i < nbPairs; i++) {
			const PxContactPair& cp = pairs[i];
			if (!(cp.events.isSet(PxPairFlag::eNOTIFY_TOUCH_FOUND))) {
				continue;
			}

			PxContactPairPoint cpBuf[1];
			PxU32 nContacts = cp.extractContacts(cpBuf, 1);
			if (nContacts > 0) {
				ContactPair entry;
				entry.bodyA    = (RigidBodyHandle)(uintptr_t)pairHeader.actors[0]->userData;
				entry.bodyB    = (RigidBodyHandle)(uintptr_t)pairHeader.actors[1]->userData;
				entry.aIsStatic = pairHeader.actors[0]->is<PxRigidStatic>() != nullptr;
				entry.bIsStatic = pairHeader.actors[1]->is<PxRigidStatic>() != nullptr;
				entry.position = Vec3(cpBuf[0].position.x, cpBuf[0].position.y, cpBuf[0].position.z);
				contacts.push_back(entry);
			}
		}
	}

	void onTrigger(PxTriggerPair*, PxU32) override {}
	void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
	void onWake(PxActor**, PxU32) override {}
	void onSleep(PxActor**, PxU32) override {}
	void onAdvance(const PxRigidBody* const*, const PxTransform*, const PxU32) override {}
};

// ===================================================================
// Custom filter shader — wraps default and enables contact reports
// ===================================================================
static PxFilterFlags contactReportFilterShader(
	PxFilterObjectAttributes attributes0, PxFilterData filterData0,
	PxFilterObjectAttributes attributes1, PxFilterData filterData1,
	PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize)
{
	pairFlags = PxPairFlag::eSOLVE_CONTACT | PxPairFlag::eDETECT_DISCRETE_CONTACT
	          | PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_CONTACT_POINTS;
	return PxFilterFlag::eDEFAULT;
}

// ===================================================================
// Impl
// ===================================================================

struct PhysXWorldBackend::Impl {
	EngineErrorCallback          errorCallback;
	SimulationEventCallback      simCallback;
	PxDefaultAllocator           allocator;
	PxFoundation*          foundation    = nullptr;
	PxPhysics*             physics       = nullptr;
	PxScene*               scene         = nullptr;
	PxMaterial*            material      = nullptr;
	PxDefaultCpuDispatcher* cpuDispatcher = nullptr;
	PxPvd*                 pvd           = nullptr;
	Vec3                   gravity       = Vec3(0, -9.81f, 0);
	bool                   initialized   = false;
};

PhysXWorldBackend::PhysXWorldBackend() : m_impl(std::make_unique<Impl>()) {}
PhysXWorldBackend::~PhysXWorldBackend() { shutdown(); }

Result<void, PhysicsError> PhysXWorldBackend::initialize(const PhysicsWorldDesc& desc) {
	auto& p = *m_impl;

	p.foundation = PxCreateFoundation(PX_PHYSICS_VERSION, p.allocator, p.errorCallback);
	if (!p.foundation) {
		EError("Physics: PxCreateFoundation failed");
		return PhysicsError::InitFailed;
	}

#ifdef EE_DEBUG
	// PVD debug connection
	p.pvd = PxCreatePvd(*p.foundation);
	if (p.pvd) {
		PxPvdTransport* transport = PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10);
		if (transport) {
			p.pvd->connect(*transport, PxPvdInstrumentationFlag::eALL);
		}
	}
#endif

	PxTolerancesScale scale;
	p.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *p.foundation, scale, true, p.pvd);
	if (!p.physics) {
		EError("Physics: PxCreatePhysics failed");
		return PhysicsError::InitFailed;
	}

	if (!PxInitExtensions(*p.physics, p.pvd)) {
		EError("Physics: PxInitExtensions failed");
		return PhysicsError::InitFailed;
	}

	p.material = p.physics->createMaterial(0.5f, 0.5f, 0.3f);

	// CPU dispatcher (required for scene)
	p.cpuDispatcher = PxDefaultCpuDispatcherCreate(std::thread::hardware_concurrency());

	PxSceneDesc sceneDesc(scale);
	sceneDesc.gravity       = PxVec3(desc.gravity.x, desc.gravity.y, desc.gravity.z);
	sceneDesc.cpuDispatcher = p.cpuDispatcher;
	sceneDesc.filterShader  = contactReportFilterShader;
	sceneDesc.flags        |= PxSceneFlag::eENABLE_CCD;

	p.scene = p.physics->createScene(sceneDesc);
	if (!p.scene) {
		EError("Physics: createScene failed");
		return PhysicsError::InitFailed;
	}

	p.scene->setSimulationEventCallback(&p.simCallback);

#ifdef EE_DEBUG
	if (p.pvd) {
		p.scene->getScenePvdClient()->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS, true);
		p.scene->getScenePvdClient()->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONTACTS, true);
		p.scene->getScenePvdClient()->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES, true);
	}
#endif

	// Ground plane at y=-20, normal (0,1,0)
	PxRigidStatic* ground = PxCreatePlane(*p.physics, PxPlane(0, 1, 0, 20), *p.material);
	if (ground) p.scene->addActor(*ground);

	p.gravity = desc.gravity;
	p.initialized = true;

	EInfo("Physics: PhysX 5 world ready (gravity={:.2f},{:.2f},{:.2f} maxBodies={})",
		p.gravity.x, p.gravity.y, p.gravity.z, desc.maxBodies);
	return {};
}

void PhysXWorldBackend::shutdown() {
	auto& p = *m_impl;
	if (p.scene)          { p.scene->release(); p.scene = nullptr; }
	if (p.cpuDispatcher)  { p.cpuDispatcher->release(); p.cpuDispatcher = nullptr; }
	if (p.material)       { p.material->release(); p.material = nullptr; }
	if (p.physics)        { PxCloseExtensions(); p.physics->release(); p.physics = nullptr; }
	if (p.pvd)            { p.pvd->release(); p.pvd = nullptr; }
	if (p.foundation)     { p.foundation->release(); p.foundation = nullptr; }
	p.initialized = false;
}

Result<void, PhysicsError> PhysXWorldBackend::step(F32 dt) {
	auto& p = *m_impl;
	if (!p.scene) return PhysicsError::StepFailed;
	p.scene->simulate(dt);
	p.scene->fetchResults(true);
	return {};
}

void PhysXWorldBackend::setGravity(const Vec3& g) {
	m_impl->gravity = g;
	if (m_impl->scene)
		m_impl->scene->setGravity(PxVec3(g.x, g.y, g.z));
}

Vec3 PhysXWorldBackend::gravity() const { return m_impl->gravity; }
bool PhysXWorldBackend::isInitialized() const { return m_impl->initialized; }
void* PhysXWorldBackend::sdk() const { return m_impl->physics; }
void* PhysXWorldBackend::scene() const { return m_impl->scene; }

Vector<ContactPair> PhysXWorldBackend::fetchContacts() {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.simCallback.mutex);
	Vector<ContactPair> result = std::move(p.simCallback.contacts);
	p.simCallback.contacts.clear();
	return result;
}

RaycastHit PhysXWorldBackend::raycast(const Vec3& origin, const Vec3& dir, F32 maxDist, const RaycastFilter& filter) const {
	RaycastHit result;
	if (!m_impl->scene) return result;
	PxVec3 o(origin.x, origin.y, origin.z);
	PxVec3 d(dir.x, dir.y, dir.z);
	d = d.getNormalized();
	PxQueryFilterData fd;
	fd.flags = filter.staticOnly ? PxQueryFlag::eSTATIC : (PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC);
	if (filter.ignoreBody != InvalidRigidBody) {
		// We can't easily map handle→PxRigidActor* here, so we skip filtering for now
		// Future: add reverse map in body backend
	}
	PxRaycastBuffer hit;
	if (m_impl->scene->raycast(o, d, maxDist, hit, PxHitFlags(PxHitFlag::eDEFAULT), fd)) {
		result.hit = true;
		result.position = Vec3(hit.block.position.x, hit.block.position.y, hit.block.position.z);
		result.normal   = Vec3(hit.block.normal.x, hit.block.normal.y, hit.block.normal.z);
		result.distance = hit.block.distance;
		if (hit.block.actor)
			result.body = (RigidBodyHandle)(uintptr_t)hit.block.actor->userData;
	}
	return result;
}

static PxQueryFilterData s_queryFilter;
OverlapResult PhysXWorldBackend::overlapBox(const Vec3& center, const Vec3& halfExtents) const {
	OverlapResult result;
	if (!m_impl->scene) return result;
	PxBoxGeometry geom(halfExtents.x, halfExtents.y, halfExtents.z);
	PxTransform pose(PxVec3(center.x, center.y, center.z));
	PxOverlapHit hitBuf[32];
	PxOverlapBuffer buf(hitBuf, 32);
	PxQueryFilterData fd; fd.flags = PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC;
	if (m_impl->scene->overlap(geom, pose, buf, fd))
			for (PxU32 i = 0; i < buf.getNbAnyHits(); i++)
				if (buf.getAnyHit(i).actor)
					result.bodies.push_back((RigidBodyHandle)(uintptr_t)buf.getAnyHit(i).actor->userData);
	return result;
}

OverlapResult PhysXWorldBackend::overlapSphere(const Vec3& center, F32 radius) const {
	OverlapResult result;
	if (!m_impl->scene) return result;
	PxSphereGeometry geom(radius);
	PxTransform pose(PxVec3(center.x, center.y, center.z));
	PxOverlapHit hitBuf[32];
	PxOverlapBuffer buf(hitBuf, 32);
	PxQueryFilterData fd; fd.flags = PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC;
	if (m_impl->scene->overlap(geom, pose, buf, fd))
		for (PxU32 i = 0; i < buf.getNbAnyHits(); i++)
			if (buf.getAnyHit(i).actor)
				result.bodies.push_back((RigidBodyHandle)(uintptr_t)buf.getAnyHit(i).actor->userData);
	return result;
}

} // namespace PhysX5
EE_NAMESPACE_PHYSICS_END

#endif // EE_PHYSICS_BACKEND_PHYSX5
