#pragma once

#include "Macros.h"
#include "Guid.hpp"

EE_NAMESPACE_BEGIN

/**
 * @brief The fundamental engine entity.
 *
 * Object stores only a Guid. Different Object instances with the same Guid
 * represent the same logical object. Business data is owned by Subsystems.
 */
class EE_API Object {
public:
	/// @brief Construct a new Object with a freshly generated Guid.
	Object();

	/**
	 * @brief Construct an Object from an existing Guid.
	 * @param guid The Guid identifying this object.
	 */
	explicit Object(const Guid& guid);

	EE_DEFAULT_COPY(Object)
	EE_DEFAULT_MOVE(Object)
	~Object() = default;

	/**
	 * @brief Get the Guid identifying this object.
	 * @return Const reference to the object's Guid.
	 */
	EE_NODISCARD const Guid& guid() const { return m_guid; }

	EE_NODISCARD bool operator==(const Object& other) const { return m_guid == other.m_guid; }
	EE_NODISCARD bool operator!=(const Object& other) const { return m_guid != other.m_guid; }
	EE_NODISCARD bool operator<(const Object& other) const { return m_guid < other.m_guid; }

private:
	Guid m_guid;
};

EE_NAMESPACE_END

namespace std {

template <>
struct hash<EnderEngine::Object> {
	size_t operator()(const EnderEngine::Object& object) const {
		return std::hash<EnderEngine::Guid>{}(object.guid());
	}
};

} // namespace std
