#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#define EVF_NAMESPACE_BEGIN namespace EnderVFiles2 {
#define EVF_NAMESPACE_END }

#define EVF_VERSION_MAJOR 2
#define EVF_VERSION_MINOR 0
#define EVF_VERSION_PATCH 0
#define EVF_MAKEVERSION(major, minor, patch) ((major << 4) | (minor << 2) | patch)
#define EVF_VERSION EVF_MAKEVERSION(EVF_VERSION_MAJOR, EVF_VERSION_MINOR, EVF_VERSION_PATCH)

#define EVF_INFO(...) printf(__VA_ARGS__)
#define EVF_ERROR(...) fprintf(stderr, __VA_ARGS__)

EVF_NAMESPACE_BEGIN

using UInt8 = std::uint8_t;
using Int8 = std::int8_t;
using UInt16 = std::uint16_t;
using Int16 = std::int16_t;
using UInt32 = std::uint32_t;
using Int32 = std::int32_t;
using UInt64 = std::uint64_t;
using Int64 = std::int64_t;
using Size = std::size_t;

using String = std::string;
using StringView = std::string_view;
using Path = std::filesystem::path;

template <typename T>
using Vector = std::vector<T>;
template <typename T, Size size>
using Array = std::array<T, size>;

/*
 * @brief Packer for EnderVFiles2. When this object destructs, it will start pack.
 */
class Packer {
public:
	struct Config {
		String name;
		String filename;
		String password;
		Path   AssetDir;
		Path   OutputDir;
	};

	Packer() = default;
	~Packer();

	Packer& setName(const String& name) { m_config.name = name; return *this; }
	Packer& setFileName(const String& name) { m_config.filename = name; return *this; }
	Packer& setPassword(const String& password) { m_config.password = password; return *this; }
	Packer& setAssetDir(const Path& path);
	Packer& setOutputDir(const Path& path);

	void pack(); // pack manually

private:
	Config m_config;
	bool m_packed = false;
};

class Reader {

};

class VirtualFileSystem {
public:
	static UInt8 getEVFVersion() { return EVF_VERSION; }
	static const char* getOpenSSLVersion();
};

using VFS = VirtualFileSystem;

EVF_NAMESPACE_END