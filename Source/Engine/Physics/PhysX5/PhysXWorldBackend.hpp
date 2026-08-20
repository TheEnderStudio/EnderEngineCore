#pragma once

#include <Core/Types.hpp>
#include <Physics/PhysicsTypes.hpp>
#include <Physics/Errors.hpp>

EE_NAMESPACE_PHYSICS_BEGIN
namespace PhysX5 {

class PhysXWorldBackend {
public:
	PhysXWorldBackend();
	~PhysXWorldBackend();

	EE_NO_COPY(PhysXWorldBackend)

	Result<void, PhysicsError> initialize(const PhysicsWorldDesc& desc);
	void shutdown();

	Result<void, PhysicsError> step(F32 dt);

	void setGravity(const Vec3& g);
	Vec3 gravity() const;
	bool isInitialized() const;

	RaycastHit raycast(const Vec3& origin, const Vec3& dir, F32 maxDist, const RaycastFilter& filter) const;
	OverlapResult overlapBox(const Vec3& center, const Vec3& halfExtents) const;
	OverlapResult overlapSphere(const Vec3& center, F32 radius) const;

	/// @brief Get the PxPhysics* for creating actors (internal use by body backend).
	void* sdk() const;
	/// @brief Get the PxScene* for adding actors (internal use by body backend).
	void* scene() const;

	/// @brief Retrieve contact pairs from the last simulation step, then clear internal buffer.
	Vector<ContactPair> fetchContacts();

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

} // namespace PhysX5
EE_NAMESPACE_PHYSICS_END
