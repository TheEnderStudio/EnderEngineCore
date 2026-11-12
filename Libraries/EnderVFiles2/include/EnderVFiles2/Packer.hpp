#pragma once

#include "Error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <string>

namespace EnderVFiles2
{
	enum class CompressionLevel : std::uint8_t
	{
		Store = 0,
		Fastest = 1,
		Default = 6,
		Best = 9,
	};

	struct PackerConfig
	{
		std::filesystem::path inputDirectory;
		std::filesystem::path outputDirectory;
		std::string packageName;
		std::string password;
		std::uint64_t volumeSize = 1024ull * 1024ull * 1024ull;
		std::uint64_t compressionThresholdBytes = 64ull * 1024ull * 1024ull;
		std::uint32_t kdfIterations = 300000;
		std::size_t ioBufferSize = 1024ull * 1024ull;
		std::size_t bufferAlignment = 64;
		std::pmr::memory_resource* memoryResource = nullptr;
		CompressionLevel compressionLevel = CompressionLevel::Default;
		bool overwriteExisting = false;
	};

	struct PackResult
	{
		std::filesystem::path indexPath;
		std::uint64_t totalUncompressedBytes = 0;
		std::uint64_t totalStoredBytes = 0;
		std::uint32_t fileCount = 0;
		std::uint32_t directoryCount = 0;
		std::uint32_t volumeCount = 0;
		std::uint32_t skippedEntries = 0;
	};

	class Packer
	{
	public:
		explicit Packer(PackerConfig config);
		Packer(const Packer&) = delete;
		Packer& operator=(const Packer&) = delete;
		Packer(Packer&&) noexcept;
		Packer& operator=(Packer&&) noexcept;
		~Packer();

		Result<PackResult> pack();

	private:
		PackerConfig m_config;
	};
}
