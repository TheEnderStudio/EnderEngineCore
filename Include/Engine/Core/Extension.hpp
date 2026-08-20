#pragma once

#include "Types.hpp"
#include "Version.hpp"

#ifndef EE_WINDOWS
#error "Platform is unsupported now"
#endif

EE_NAMESPACE_BEGIN

constexpr UInt16 ExtensionMagicNumber = 0x6a43;
constexpr UInt16 CurrentExtensionApiVersion = 1;

/// @brief Base class for engine extensions. Override onLoad/onShutdown for custom behavior.
class EE_API Extension {
public:
	UInt16 magicNum = ExtensionMagicNumber;

public:
	/// @brief Construct an extension with name, version, and author.
	Extension(const String& name, const Version& version, const String& author) :
		m_name(name), m_version(version), m_author(author) {
	}

	/// @brief Get the extension display name.
	const String& getName() const { return m_name; }
	/// @brief Get the extension version.
	const Version& getVersion() const { return m_version; }
	/// @brief Get the extension author name.
	const String& getAuthor() const { return m_author; }
	/// @brief Get the extension API version this extension was compiled against.
	UInt16 getApiVersion() const { return m_apiVer; }

protected:
	/// @brief Called when the extension is loaded by the loader.
	virtual void onLoad() {};
	/// @brief Called when the extension is about to be unloaded.
	virtual void onShutdown() {};

	friend class ExtensionLoader;

private:
	UInt16 m_apiVer = CurrentExtensionApiVersion;
	String m_name;
	Version m_version;
	String m_author;
};

/// @brief Loads and manages engine extensions. Static helper for Extension lifecycle.
class EE_API ExtensionLoader {
public:
	/// @brief Initialize the extension loading subsystem.
	static void initialize();
	/// @brief Shut down the extension loading subsystem.
	static void shutdown();

	/// @brief Load an extension by name. Returns the loaded Extension, or nullptr on failure.
	static Extension* load(const String& name);
};

EE_NAMESPACE_END