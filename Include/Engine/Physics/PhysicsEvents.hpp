#pragma once

#include <Core/Types.hpp>
#include <Core/Event.hpp>

EE_NAMESPACE_PHYSICS_BEGIN

/// @brief Event fired at the beginning of a physics simulation step.
class PhysicsStepBeginEvent : public Event {
public:
	PhysicsStepBeginEvent() = default;
	EE_DEFAULT_COPY(PhysicsStepBeginEvent)
	EE_DEFAULT_MOVE(PhysicsStepBeginEvent)
	F64 deltaTime = 0.0;
};

/// @brief Event fired at the end of a physics simulation step.
class PhysicsStepEndEvent : public Event {
public:
	PhysicsStepEndEvent() = default;
	EE_DEFAULT_COPY(PhysicsStepEndEvent)
	EE_DEFAULT_MOVE(PhysicsStepEndEvent)
	F64 deltaTime = 0.0;
};

/// @brief Event fired when two bodies come into contact.
class EE_API PhysicsContactEvent : public Event {
public:
	PhysicsContactEvent() = default;
	EE_DEFAULT_COPY(PhysicsContactEvent)
	EE_DEFAULT_MOVE(PhysicsContactEvent)
	ContactPair pair;
};

EE_NAMESPACE_PHYSICS_END
