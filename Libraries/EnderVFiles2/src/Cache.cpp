#include "Detail/Cache.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace EnderVFiles2::Detail
{
	namespace
	{
		constexpr std::size_t MaxCacheEntries = 65536;
	}

	Cache::Cache(std::pmr::memory_resource* memoryResource, std::size_t capacityBytes)
		: m_memoryResource(memoryResource)
		, m_capacityBytes(capacityBytes)
		, m_lru(memoryResource)
		, m_lookup(memoryResource)
	{
	}

	SharedBuffer Cache::find(std::string_view key)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto iterator = m_lookup.find(key);
		if (iterator == m_lookup.end())
		{
			m_misses.fetch_add(1, std::memory_order_relaxed);
			return {};
		}

		m_lru.splice(m_lru.begin(), m_lru, iterator->second);
		m_hits.fetch_add(1, std::memory_order_relaxed);
		return iterator->second->buffer;
	}

	void Cache::insert(std::string_view key, SharedBuffer buffer)
	{
		if (buffer == nullptr)
		{
			return;
		}

		const std::size_t bufferSize = buffer->size();

		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_capacityBytes == 0 || bufferSize > m_capacityBytes)
		{
			return;
		}

		auto existing = m_lookup.find(key);
		if (existing != m_lookup.end())
		{
			m_cachedBytes -= existing->second->buffer->size();
			m_lru.erase(existing->second);
			m_lookup.erase(existing);
		}

		while (!m_lru.empty()
			&& (m_cachedBytes + bufferSize > m_capacityBytes || m_lru.size() >= MaxCacheEntries))
		{
			const CacheNode& evicted = m_lru.back();
			m_cachedBytes -= evicted.buffer->size();
			m_lookup.erase(std::string_view(evicted.key.data(), evicted.key.size()));
			m_lru.pop_back();
		}

		m_lru.emplace_front(m_memoryResource);
		CacheNode& node = m_lru.front();
		if (!key.empty())
		{
			node.key.assign(key.data(), key.size());
		}
		node.buffer = std::move(buffer);
		m_lookup.emplace(std::string_view(node.key.data(), node.key.size()), m_lru.begin());
		m_cachedBytes += bufferSize;
	}

	void Cache::setCapacity(std::size_t capacityBytes) noexcept
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_capacityBytes = capacityBytes;
		while (!m_lru.empty()
			&& (m_cachedBytes > m_capacityBytes || m_lru.size() > MaxCacheEntries))
		{
			const CacheNode& evicted = m_lru.back();
			m_cachedBytes -= evicted.buffer->size();
			m_lookup.erase(std::string_view(evicted.key.data(), evicted.key.size()));
			m_lru.pop_back();
		}
	}

	void Cache::clear() noexcept
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_lookup.clear();
		m_lru.clear();
		m_cachedBytes = 0;
	}

	std::uint64_t Cache::hits() const noexcept
	{
		return m_hits.load(std::memory_order_relaxed);
	}

	std::uint64_t Cache::misses() const noexcept
	{
		return m_misses.load(std::memory_order_relaxed);
	}

	std::uint64_t Cache::bytes() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return static_cast<std::uint64_t>(m_cachedBytes);
	}

	std::uint64_t Cache::entries() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return static_cast<std::uint64_t>(m_lru.size());
	}
}
