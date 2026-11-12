#pragma once

#include "Buffer.hpp"
#include "Types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory_resource>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace EnderVFiles2::Detail
{
	class Cache
	{
	public:
		Cache(std::pmr::memory_resource* memoryResource, std::size_t capacityBytes);

		SharedBuffer find(std::string_view key);
		void insert(std::string_view key, SharedBuffer buffer);
		void setCapacity(std::size_t capacityBytes) noexcept;
		void clear() noexcept;

		std::uint64_t hits() const noexcept;
		std::uint64_t misses() const noexcept;
		std::uint64_t bytes() const;
		std::uint64_t entries() const;

	private:
		struct CacheNode
		{
			explicit CacheNode(std::pmr::memory_resource* memoryResource)
				: key(memoryResource)
			{
			}

			std::pmr::string key;
			SharedBuffer buffer;
		};

		using CacheList = std::pmr::list<CacheNode>;
		using CacheMap = std::pmr::unordered_map<std::string_view, typename CacheList::iterator>;

		std::pmr::memory_resource* m_memoryResource = nullptr;
		std::size_t m_capacityBytes = 0;
		std::size_t m_cachedBytes = 0;
		CacheList m_lru;
		CacheMap m_lookup;
		mutable std::mutex m_mutex;
		std::atomic<std::uint64_t> m_hits{ 0 };
		std::atomic<std::uint64_t> m_misses{ 0 };
	};
}
