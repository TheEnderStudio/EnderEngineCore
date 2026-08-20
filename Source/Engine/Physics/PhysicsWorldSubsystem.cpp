#include <Physics/PhysicsWorldSubsystem.hpp>
#include <Core/Log.hpp>

#ifdef EE_PHYSICS_BACKEND_PHYSX5
# include "PhysX5/PhysXWorldBackend.hpp"
# define BACKEND_NS PhysX5
# define BACKEND_CLASS PhysXWorldBackend
#else
# include "Jolt/JoltWorldBackend.hpp"
# define BACKEND_NS Jolt
# define BACKEND_CLASS JoltWorldBackend
#endif

EE_NAMESPACE_PHYSICS_BEGIN

struct PhysicsWorldSubsystem::Impl {
	BACKEND_NS::BACKEND_CLASS backend;
};

PhysicsWorldSubsystem::PhysicsWorldSubsystem() : Subsystem("PhysicsWorld"), m_impl(std::make_unique<Impl>()) {}
PhysicsWorldSubsystem::~PhysicsWorldSubsystem() = default;

Result<void, PhysicsError> PhysicsWorldSubsystem::initialize(const PhysicsWorldDesc& desc) {
	return m_impl->backend.initialize(desc);
}

void PhysicsWorldSubsystem::start() {}
void PhysicsWorldSubsystem::stop() {}
bool PhysicsWorldSubsystem::isRunning() const { return false; }

Result<void, PhysicsError> PhysicsWorldSubsystem::step(F32 dt) {
	{PhysicsStepBeginEvent ev; ev.deltaTime = static_cast<F64>(dt); emit(ev);}
	auto r = m_impl->backend.step(dt);
	if (r.isOk()) {
		auto contacts = m_impl->backend.fetchContacts();
		static int once = 3; // log first 3 steps
		if (once > 0) { EInfo("Physics step: {} contacts fetched", contacts.size()); once--; }
		for (auto& cp : contacts) {
			PhysicsContactEvent ev; ev.pair = cp; emit(ev);
		}
		PhysicsStepEndEvent ev; ev.deltaTime = static_cast<F64>(dt); emit(ev);
	}
	return r;
}

void PhysicsWorldSubsystem::setGravity(const Vec3& g) { m_impl->backend.setGravity(g); }
Vec3 PhysicsWorldSubsystem::gravity() const { return m_impl->backend.gravity(); }

#ifdef EE_PHYSICS_BACKEND_PHYSX5
void* PhysicsWorldSubsystem::sdk() const { return m_impl->backend.sdk(); }
void* PhysicsWorldSubsystem::scene() const { return m_impl->backend.scene(); }

RaycastHit PhysicsWorldSubsystem::raycast(const Vec3& o, const Vec3& d, F32 m, const RaycastFilter& f) const { return m_impl->backend.raycast(o, d, m, f); }
OverlapResult PhysicsWorldSubsystem::overlapBox(const Vec3& c, const Vec3& h) const { return m_impl->backend.overlapBox(c, h); }
OverlapResult PhysicsWorldSubsystem::overlapSphere(const Vec3& c, F32 r) const { return m_impl->backend.overlapSphere(c, r); }
#else
void* PhysicsWorldSubsystem::bodyInterface() const { return m_impl->backend.bodyInterface(); }
#endif

Vector<ContactPair> PhysicsWorldSubsystem::fetchContacts() { return m_impl->backend.fetchContacts(); }

Result<void, CoreError> PhysicsWorldSubsystem::onInitialize() { return {}; }
void PhysicsWorldSubsystem::onShutdown() { m_impl->backend.shutdown(); }
void PhysicsWorldSubsystem::onUpdate(F64) {}
bool PhysicsWorldSubsystem::onRecover() { return false; }

EE_NAMESPACE_PHYSICS_END
