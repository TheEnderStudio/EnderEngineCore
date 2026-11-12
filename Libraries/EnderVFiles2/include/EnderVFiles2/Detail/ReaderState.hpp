#pragma once

#include "Cache.hpp"
#include "Crypto.hpp"
#include "Index.hpp"
#include "NativeFile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace EnderVFiles2::Detail
{
	struct Volume
	{
		NativeFile file;
		std::uint64_t size = 0;
	};

	struct ReaderState
	{
		ReaderState(std::pmr::memory_resource* memoryResourceValue, std::size_t cacheCapacityBytes)
			: memoryResource(memoryResourceValue)
			, index(memoryResourceValue)
			, volumes(memoryResourceValue)
			, cache(memoryResourceValue, cacheCapacityBytes)
		{
		}

		~ReaderState()
		{
			secureZero(std::span<Byte>(masterKey.data(), masterKey.size()));
		}

		ReaderState(const ReaderState&) = delete;
		ReaderState& operator=(const ReaderState&) = delete;

		std::pmr::memory_resource* memoryResource = nullptr;
		std::size_t bufferAlignment = 64;
		bool verifyChecksums = true;
		std::uint64_t maxFileBytes = 0;
		bool encryptedContent = false;
		bool encryptedIndex = false;
		bool compressedIndex = false;
		std::array<Byte, 16> packageUuid{};
		AesGcmKey masterKey{};
		Index index;
		std::pmr::vector<Volume> volumes;
		Cache cache;
	};
}
