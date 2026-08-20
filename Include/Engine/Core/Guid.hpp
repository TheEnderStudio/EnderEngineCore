#pragma once

#include "Macros.h"
#include "Types.hpp"

#include <uuid_v4.h>

EE_NAMESPACE_BEGIN

/**
 * @brief Globally unique identifier wrapping UUIDv4.
 *
 * Guid is a 128-bit value that identifies an Object. Two Guid instances
 * compare equal if and only if their underlying UUID bytes match.
 */
class EE_API Guid {
public:
	/// @brief Construct a nil Guid (all zeros).
	Guid();

	/// @brief Construct a Guid from a raw UUIDv4 value.
	explicit Guid(const UUIDv4::UUID& uuid);

	EE_DEFAULT_COPY(Guid)
	EE_DEFAULT_MOVE(Guid)
	~Guid() = default;

	/**
	 * @brief Generate a new random Guid.
	 * @return A newly generated Guid.
	 */
	static Guid generate();

	/**
	 * @brief Parse a Guid from its string representation.
	 * @param str The UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000").
	 * @return Parsed Guid, or NullOpt if the string is invalid.
	 */
	static Optional<Guid> parse(StringView str);

	/**
	 * @brief Convert the Guid to its canonical string form.
	 * @return UUID string including dashes.
	 */
	EE_NODISCARD String toString() const;

	/**
	 * @brief Check whether this is the nil Guid.
	 * @return true if all bytes are zero.
	 */
	EE_NODISCARD bool isNil() const;

	EE_NODISCARD bool operator==(const Guid& other) const;
	EE_NODISCARD bool operator!=(const Guid& other) const;
	EE_NODISCARD bool operator<(const Guid& other) const;

	/**
	 * @brief Access the underlying UUIDv4 value.
	 * @return Const reference to the internal UUID.
	 */
	EE_NODISCARD const UUIDv4::UUID& raw() const { return m_uuid; }

private:
	UUIDv4::UUID m_uuid;
};

EE_NAMESPACE_END

namespace std {

template <>
struct hash<EnderEngine::Guid> {
	size_t operator()(const EnderEngine::Guid& guid) const {
		return std::hash<UUIDv4::UUID>{}(guid.raw());
	}
};

} // namespace std
