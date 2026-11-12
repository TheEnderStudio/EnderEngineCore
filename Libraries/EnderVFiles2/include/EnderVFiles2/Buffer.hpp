#pragma once

#include "Types.hpp"

#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>

namespace EnderVFiles2
{
	inline constexpr std::size_t DefaultBufferAlignment = 64;

	class Buffer
	{
	public:
		Buffer() noexcept = default;

		Buffer(std::pmr::memory_resource* memoryResource, std::size_t size, std::size_t alignment = DefaultBufferAlignment)
			: m_memoryResource(memoryResource != nullptr ? memoryResource : std::pmr::new_delete_resource())
			, m_alignment(alignment == 0 ? DefaultBufferAlignment : std::bit_ceil(alignment))
		{
			if (size > 0)
			{
				m_data = static_cast<Byte*>(m_memoryResource->allocate(size, m_alignment));
				m_capacity = size;
				m_size = size;
			}
		}

		Buffer(Buffer&& other) noexcept
			: m_memoryResource(other.m_memoryResource)
			, m_data(other.m_data)
			, m_size(other.m_size)
			, m_capacity(other.m_capacity)
			, m_alignment(other.m_alignment)
		{
			other.m_memoryResource = nullptr;
			other.m_data = nullptr;
			other.m_size = 0;
			other.m_capacity = 0;
			other.m_alignment = 1;
		}

		Buffer& operator=(Buffer&& other) noexcept
		{
			if (this != &other)
			{
				release();
				m_memoryResource = other.m_memoryResource;
				m_data = other.m_data;
				m_size = other.m_size;
				m_capacity = other.m_capacity;
				m_alignment = other.m_alignment;
				other.m_memoryResource = nullptr;
				other.m_data = nullptr;
				other.m_size = 0;
				other.m_capacity = 0;
				other.m_alignment = 1;
			}
			return *this;
		}

		Buffer(const Buffer&) = delete;
		Buffer& operator=(const Buffer&) = delete;

		~Buffer()
		{
			release();
		}

		Byte* data() noexcept
		{
			return m_data;
		}

		const Byte* data() const noexcept
		{
			return m_data;
		}

		std::size_t size() const noexcept
		{
			return m_size;
		}

		std::size_t capacity() const noexcept
		{
			return m_capacity;
		}

		std::size_t alignment() const noexcept
		{
			return m_alignment;
		}

		std::pmr::memory_resource* memoryResource() const noexcept
		{
			return m_memoryResource;
		}

		bool empty() const noexcept
		{
			return m_size == 0;
		}

		std::span<Byte> bytes() noexcept
		{
			return m_data != nullptr ? std::span<Byte>(m_data, m_size) : std::span<Byte>();
		}

		std::span<const Byte> bytes() const noexcept
		{
			return m_data != nullptr ? std::span<const Byte>(m_data, m_size) : std::span<const Byte>();
		}

		void resize(std::size_t newSize)
		{
			if (newSize == m_size)
			{
				return;
			}

			if (newSize == 0)
			{
				release();
				return;
			}

			if (newSize <= m_capacity)
			{
				m_size = newSize;
				return;
			}

			Buffer replacement(m_memoryResource, newSize, m_alignment);
			if (m_size > 0)
			{
				std::memcpy(replacement.m_data, m_data, m_size);
			}
			*this = std::move(replacement);
		}

		void reset() noexcept
		{
			release();
		}

	private:
		void release() noexcept
		{
			if (m_data != nullptr && m_memoryResource != nullptr)
			{
				m_memoryResource->deallocate(m_data, m_capacity, m_alignment);
			}
			m_memoryResource = nullptr;
			m_data = nullptr;
			m_size = 0;
			m_capacity = 0;
			m_alignment = 1;
		}

		std::pmr::memory_resource* m_memoryResource = nullptr;
		Byte* m_data = nullptr;
		std::size_t m_size = 0;
		std::size_t m_capacity = 0;
		std::size_t m_alignment = 1;
	};

	using SharedBuffer = std::shared_ptr<const Buffer>;
}
