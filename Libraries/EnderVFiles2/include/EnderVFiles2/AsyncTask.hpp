#pragma once

#include "Error.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <utility>

namespace EnderVFiles2
{
	namespace Detail
	{
		template <typename Value>
		struct AsyncState
		{
			std::mutex mutex;
			std::condition_variable condition;
			std::optional<Value> value;
			Error error;
			bool completed = false;
			bool consumed = false;

			void complete(Result<Value>&& result)
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (completed)
				{
					return;
				}

				if (result.hasValue())
				{
					value.emplace(std::move(result).value());
				}
				else if (result.error().code != ErrorCode::None)
				{
					error = std::move(result).error();
				}
				else
				{
					error = makeError(ErrorCode::InternalFailure, "Async operation produced no value and no error");
				}

				completed = true;
				condition.notify_all();
			}
		};

		template <typename Value>
		std::shared_ptr<AsyncState<Value>> makeAsyncState(std::pmr::memory_resource* memoryResource)
		{
			std::pmr::memory_resource* resource = memoryResource != nullptr ? memoryResource : std::pmr::new_delete_resource();
			return std::allocate_shared<AsyncState<Value>>(std::pmr::polymorphic_allocator<AsyncState<Value>>(resource));
		}
	}

	template <typename Value>
	class AsyncTask
	{
	public:
		using ValueType = Value;

		AsyncTask() noexcept = default;

		explicit AsyncTask(std::shared_ptr<Detail::AsyncState<Value>> state) noexcept
			: m_state(std::move(state))
		{
		}

		bool valid() const noexcept
		{
			return m_state != nullptr;
		}

		bool isReady() const
		{
			if (m_state == nullptr)
			{
				return true;
			}

			std::lock_guard<std::mutex> lock(m_state->mutex);
			return m_state->completed;
		}

		void wait() const
		{
			if (m_state == nullptr)
			{
				return;
			}

			std::unique_lock<std::mutex> lock(m_state->mutex);
			m_state->condition.wait(lock, [this] { return m_state->completed; });
		}

		Result<Value> get()
		{
			if (m_state == nullptr)
			{
				return makeError(ErrorCode::InvalidState, "AsyncTask is not valid");
			}

			std::unique_lock<std::mutex> lock(m_state->mutex);
			m_state->condition.wait(lock, [this] { return m_state->completed; });

			if (m_state->consumed)
			{
				return makeError(ErrorCode::InvalidState, "AsyncTask result has already been consumed");
			}
			m_state->consumed = true;

			if (m_state->error.code != ErrorCode::None)
			{
				return std::move(m_state->error);
			}
			if (!m_state->value.has_value())
			{
				return makeError(ErrorCode::InternalFailure, "AsyncTask completed without a value");
			}
			return std::move(*m_state->value);
		}

	private:
		std::shared_ptr<Detail::AsyncState<Value>> m_state;
	};
}
