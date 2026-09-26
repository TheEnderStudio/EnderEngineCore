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

#define EVF_INFO(...) fprintf(stdout, __VA_ARGS__)
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

namespace MagicNumbers {
#define EVF_COMMON_HEADER_MAGIC 'E', 'V', 'F', '2'
	static const UInt8 CommonHeaderMagic[4] = { EVF_COMMON_HEADER_MAGIC };
	static const UInt8 IndexFileHeaderMagic[8] = { EVF_COMMON_HEADER_MAGIC, 'I', 'N', 'D', 'x' };
	static const UInt8 VolumeFileHeaderMagic[8] = { EVF_COMMON_HEADER_MAGIC, 'V', 'O', 'L', 'm' };
#define EVF_WRAP_TO_VECTOR(array) Vector(std::begin(array), std::end(array))
}

/*
 * @brief Packer for EnderVFiles2. When this object destructs, it will start pack.
 */
class Packer {
public:
	struct Config {
		String name;
		String filename;
		String password;
		Path   assetDir;
		Path   outputDir;
		UInt64 volumeSize;
	};

	void pack(); // pack manually

	Packer() = default;
	~Packer() { pack(); }

	Packer& setName(const String& name) { m_config.name = name; return *this; }
	Packer& setFileName(const String& name) { m_config.filename = name; return *this; }
	Packer& setPassword(const String& password) { m_config.password = password; return *this; }
	Packer& setAssetDir(const Path& path);
	Packer& setOutputDir(const Path& path, bool replaceOld);
	Packer& setVolumeSize(UInt64 size) { m_config.volumeSize = size; return *this; }

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

/* static */ class IOHelper {
public:
	/* Static class doesn't need to be created and deleted */
	IOHelper() = delete;
	~IOHelper() = delete;
	void* operator new(Size) = delete;
	void  operator delete(void*) = delete;

	static bool   createFile(const Path& path);
	static bool   writeFile(const Path& path, const Vector<UInt8>& bytes);
	static UInt64 readFile(const Path& path, Vector<UInt8>& bytes, UInt64 offset = 0, UInt64 bytesToRead = UINT64_MAX);
};

EVF_NAMESPACE_END