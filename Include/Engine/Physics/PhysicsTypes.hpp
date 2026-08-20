#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_PHYSICS_BEGIN

/// @brief Handle to a physics world.
using PhysicsWorldHandle = UInt64;
/// @brief Handle to a rigid body.
using RigidBodyHandle   = UInt64;
/// @brief Handle to a collider.
using ColliderHandle    = UInt64;

/// @brief Sentinel value for an invalid rigid body handle.
static constexpr RigidBodyHandle InvalidRigidBody = 0xFFFFFFFFFFFFFFFFULL;
/// @brief Sentinel value for an invalid collider handle.
static constexpr ColliderHandle  InvalidCollider  = 0xFFFFFFFFFFFFFFFFULL;

/// @brief Type of a rigid body.
enum class RigidBodyType : UInt8 { Static, Dynamic, Kinematic };
/// @brief Primitive collider shape type.
enum class ColliderShape  : UInt8 { Box, Sphere, Capsule };

/// @brief Flags to lock angular motion around specific axes.
enum class AngularLockFlag : UInt8 {
	None   = 0,
	LockX  = 1 << 0,
	LockY  = 1 << 1,
	LockZ  = 1 << 2,
};
/// @brief Bitwise OR for combining angular lock flags.
inline AngularLockFlag operator|(AngularLockFlag a, AngularLockFlag b) { return (AngularLockFlag)((UInt8)a | (UInt8)b); }
/// @brief Bitwise AND test for angular lock flags.
inline bool operator&(AngularLockFlag a, AngularLockFlag b) { return ((UInt8)a & (UInt8)b) != 0; }

/// @brief Descriptor for creating a rigid body.
struct RigidBodyDesc {
	RigidBodyType type           = RigidBodyType::Dynamic;
	Vec3          position       = Vec3(0);
	Quat          rotation       = Quat(1,0,0,0);
	Vec3          linearVelocity = Vec3(0);
	F32           mass           = 1.0f;
	F32           friction       = 0.5f;
	F32           restitution    = 0.0f;
	F32           linearDamping  = 0.05f;
	bool          enableGravity  = true;
};

/// @brief Descriptor for creating a collider.
struct ColliderDesc {
	ColliderShape shape       = ColliderShape::Box;
	Vec3          halfExtents = Vec3(0.5f);
	F32           radius      = 0.5f;
	F32           height      = 1.0f;
	Vec3          localPos    = Vec3(0);
	Quat          localRot    = Quat(1,0,0,0);
};

/// @brief Descriptor for creating a physics world.
struct PhysicsWorldDesc {
	Vec3  gravity    = Vec3(0, -9.81f, 0);
	UInt32 maxBodies = 65536;
};

// ===================================================================
// Scene query
// ===================================================================

/// @brief Result of a raycast query.
struct RaycastHit {
	bool  hit       = false;
	Vec3  position  = Vec3(0);
	Vec3  normal    = Vec3(0);
	F32   distance  = 0.0f;
	RigidBodyHandle body = InvalidRigidBody;
};

/// @brief Filter parameters for raycast queries.
struct RaycastFilter {
	RigidBodyHandle ignoreBody = InvalidRigidBody;
	bool             staticOnly = false;
};

/// @brief Result of an overlap query.
struct OverlapResult {
	Vector<RigidBodyHandle> bodies;
};

/// @brief Describes a contact between two bodies.
struct ContactPair {
	RigidBodyHandle bodyA     = InvalidRigidBody;
	RigidBodyHandle bodyB     = InvalidRigidBody;
	Vec3            position  = Vec3(0);
	bool            aIsStatic = false;
	bool            bIsStatic = false;
};

EE_NAMESPACE_PHYSICS_END
