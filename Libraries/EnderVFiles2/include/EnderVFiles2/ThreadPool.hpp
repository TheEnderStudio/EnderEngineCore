#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace EnderVFiles2
{
	template <typename Pool>
	concept ThreadPool = requires(Pool& pool, std::function<void()> task)
	{
		{ pool.submit(std::move(task)) } -> std::same_as<void>;
	};

	class ThreadPoolRef
	{
	public:
		ThreadPoolRef() noexcept = default;
		ThreadPoolRef(const ThreadPoolRef&) noexcept = default;
		ThreadPoolRef& operator=(const ThreadPoolRef&) noexcept = default;

		template <ThreadPool Pool>
			requires (!std::same_as<std::remove_cvref_t<Pool>, ThreadPoolRef>
				&& !std::is_const_v<Pool>
				&& !std::is_volatile_v<Pool>)
		explicit ThreadPoolRef(Pool& pool) noexcept
			: m_context(std::addressof(pool))
			, m_submit(&ThreadPoolRef::submitTo<Pool>)
		{
		}

		template <ThreadPool Pool>
			requires (!std::same_as<std::remove_cvref_t<Pool>, ThreadPoolRef>
				&& !std::is_const_v<Pool>
				&& !std::is_volatile_v<Pool>)
		ThreadPoolRef& operator=(Pool& pool) noexcept
		{
			m_context = std::addressof(pool);
			m_submit = &ThreadPoolRef::submitTo<Pool>;
			return *this;
		}

		explicit operator bool() const noexcept
		{
			return m_submit != nullptr;
		}

		void submit(std::function<void()> task) const
		{
			if (m_submit != nullptr)
			{
				m_submit(m_context, std::move(task));
			}
		}

		void reset() noexcept
		{
			m_context = nullptr;
			m_submit = nullptr;
		}

	private:
		template <ThreadPool Pool>
		static void submitTo(void* context, std::function<void()> task)
		{
			static_cast<Pool*>(context)->submit(std::move(task));
		}

		void* m_context = nullptr;
		void (*m_submit)(void*, std::function<void()>) = nullptr;
	};
}
