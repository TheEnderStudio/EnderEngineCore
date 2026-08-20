#pragma once

#include "Object.hpp"
#include <spdlog/common.h>

EE_NAMESPACE_BEGIN

/// @brief Holds a semantic version (major.minor.patch) associated with a unique GUID.
class EE_API Version : public Object {
public:
	/// @brief Construct a Version with the given components and GUID.
	Version(UInt32 major, UInt32 minor, UInt32 patch, Guid guid) :
		m_major(major), m_minor(minor), m_patch(patch), Object(guid) {}

	/// @brief Get the engine's compile-time version.
	static const Version& getEngineVersion() {
		const static Version engineVer(EE_VERSION_MAJOR, EE_VERSION_MINOR, EE_VERSION_PATCH, Guid(UUIDv4::UUID::fromStrFactory(EE_VERSION_GUID)));
		return engineVer;
	}

	/// @brief Get the major version component.
	UInt32 major() const { return m_major; }
	/// @brief Get the minor version component.
	UInt32 minor() const { return m_minor; }
	/// @brief Get the patch version component.
	UInt32 patch() const { return m_patch; }
	/// @brief Return a human-readable version string.
	String toString() const {
		return fmt::format("v{}.{}.{}-{{{}}}", m_major, m_minor, m_patch, guid().toString());
	}

private:
	UInt32 m_major;
	UInt32 m_minor;
	UInt32 m_patch;
};

EE_NAMESPACE_END