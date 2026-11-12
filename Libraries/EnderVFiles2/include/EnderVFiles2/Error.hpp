#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace EnderVFiles2
{
	enum class ErrorCode : std::uint32_t
	{
		None = 0,
		InvalidArgument,
		InvalidState,
		NotMounted,
		NotFound,
		IsDirectory,
		NotAFile,
		IoFailure,
		InvalidFormat,
		UnsupportedVersion,
		CorruptIndex,
		AuthenticationFailed,
		PasswordRequired,
		DecryptionFailed,
		DecompressionFailed,
		CompressionFailed,
		CryptoFailure,
		RandomFailure,
		VolumeMissing,
		VolumeMismatch,
		FileTooLarge,
		IndexTooLarge,
		TooManyEntries,
		TooManyVolumes,
		PathTooLong,
		UnsafePath,
		DuplicateEntry,
		AllocationFailure,
		ThreadPoolUnavailable,
		ResourceLoadFailed,
		CallbackFailure,
		InternalFailure,
	};

	struct Error
	{
		ErrorCode code = ErrorCode::None;
		std::string message;

		Error() = default;

		Error(ErrorCode errorCode, std::string errorMessage = {})
			: code(errorCode)
			, message(std::move(errorMessage))
		{
		}

		explicit operator bool() const noexcept
		{
			return code != ErrorCode::None;
		}
	};

	inline Error makeError(ErrorCode code, std::string_view message = {})
	{
		return Error(code, std::string(message));
	}

	template <typename Value>
	class Result
	{
	public:
		Result(const Value& value) requires std::copy_constructible<Value>
			: m_value(value)
		{
		}

		Result(Value&& value)
			: m_value(std::move(value))
		{
		}

		Result(Error error)
			: m_error(std::move(error))
		{
		}

		bool hasValue() const noexcept
		{
			return m_value.has_value();
		}

		explicit operator bool() const noexcept
		{
			return hasValue();
		}

		Value& value() &
		{
			return *m_value;
		}

		const Value& value() const&
		{
			return *m_value;
		}

		Value&& value() &&
		{
			return std::move(*m_value);
		}

		Error& error() &
		{
			return m_error;
		}

		const Error& error() const&
		{
			return m_error;
		}

		Error&& error() &&
		{
			return std::move(m_error);
		}

	private:
		std::optional<Value> m_value;
		Error m_error;
	};

	template <>
	class Result<void>
	{
	public:
		Result() = default;

		Result(Error error)
			: m_error(std::move(error))
		{
		}

		bool hasValue() const noexcept
		{
			return m_error.code == ErrorCode::None;
		}

		explicit operator bool() const noexcept
		{
			return hasValue();
		}

		Error& error() &
		{
			return m_error;
		}

		const Error& error() const&
		{
			return m_error;
		}

		Error&& error() &&
		{
			return std::move(m_error);
		}

	private:
		Error m_error;
	};
}
