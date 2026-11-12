#include "Detail/NativeFile.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace EnderVFiles2::Detail
{
	namespace
	{
#ifdef _WIN32
		std::string lastFileError(std::string_view operation)
		{
			const DWORD code = GetLastError();
			const std::error_code errorCode(static_cast<int>(code), std::system_category());
			return std::string(operation) + ": " + errorCode.message();
		}

		bool isValidHandle(void* handle) noexcept
		{
			return handle != nullptr && handle != reinterpret_cast<void*>(INVALID_HANDLE_VALUE);
		}
#else
		std::string lastFileError(std::string_view operation)
		{
			return std::string(operation) + ": " + std::generic_category().message(errno);
		}
#endif
	}

	NativeFile::NativeFile(NativeFile&& other) noexcept
	{
#ifdef _WIN32
		m_handle = other.m_handle;
		other.m_handle = nullptr;
#else
		m_fileDescriptor = other.m_fileDescriptor;
		other.m_fileDescriptor = -1;
#endif
	}

	NativeFile& NativeFile::operator=(NativeFile&& other) noexcept
	{
		if (this != &other)
		{
			close();
#ifdef _WIN32
			m_handle = other.m_handle;
			other.m_handle = nullptr;
#else
			m_fileDescriptor = other.m_fileDescriptor;
			other.m_fileDescriptor = -1;
#endif
		}
		return *this;
	}

	NativeFile::~NativeFile()
	{
		close();
	}

	Result<NativeFile> NativeFile::openRead(const std::filesystem::path& path)
	{
#ifdef _WIN32
		const HANDLE handle = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
			nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("CreateFileW(read)"));
		}

		NativeFile file;
		file.m_handle = reinterpret_cast<void*>(handle);
		return std::move(file);
#else
		int flags = O_RDONLY;
#ifdef O_CLOEXEC
		flags |= O_CLOEXEC;
#endif
		const int descriptor = ::open(path.c_str(), flags);
		if (descriptor < 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("open(read)"));
		}

		NativeFile file;
		file.m_fileDescriptor = descriptor;
		return std::move(file);
#endif
	}

	Result<NativeFile> NativeFile::openWrite(const std::filesystem::path& path)
	{
#ifdef _WIN32
		const HANDLE handle = CreateFileW(
			path.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("CreateFileW(write)"));
		}

		NativeFile file;
		file.m_handle = reinterpret_cast<void*>(handle);
		return std::move(file);
#else
		int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
		flags |= O_CLOEXEC;
#endif
		const int descriptor = ::open(path.c_str(), flags, 0644);
		if (descriptor < 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("open(write)"));
		}

		NativeFile file;
		file.m_fileDescriptor = descriptor;
		return std::move(file);
#endif
	}

	bool NativeFile::isValid() const noexcept
	{
#ifdef _WIN32
		return isValidHandle(m_handle);
#else
		return m_fileDescriptor >= 0;
#endif
	}

	Result<std::uint64_t> NativeFile::size() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
		if (!isValidHandle(m_handle))
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}

		LARGE_INTEGER fileSize{};
		if (GetFileSizeEx(reinterpret_cast<HANDLE>(m_handle), &fileSize) == 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("GetFileSizeEx"));
		}
		return static_cast<std::uint64_t>(fileSize.QuadPart);
#else
		if (m_fileDescriptor < 0)
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}

		struct stat status{};
		if (fstat(m_fileDescriptor, &status) != 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("fstat"));
		}
		return static_cast<std::uint64_t>(status.st_size);
#endif
	}

	Result<void> NativeFile::readAt(std::uint64_t offset, MutableByteSpan destination) const
	{
		if (destination.empty())
		{
			return {};
		}

		std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
		if (!isValidHandle(m_handle))
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}

		LARGE_INTEGER position{};
		position.QuadPart = static_cast<LONGLONG>(offset);
		if (SetFilePointerEx(reinterpret_cast<HANDLE>(m_handle), position, nullptr, FILE_BEGIN) == 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("SetFilePointerEx"));
		}

		std::size_t completed = 0;
		while (completed < destination.size())
		{
			const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(destination.size() - completed, 1024ull * 1024ull * 1024ull));
			DWORD read = 0;
			if (ReadFile(
				reinterpret_cast<HANDLE>(m_handle),
				destination.data() + completed,
				requested,
				&read,
				nullptr) == 0)
			{
				return makeError(ErrorCode::IoFailure, lastFileError("ReadFile"));
			}
			if (read == 0)
			{
				return makeError(ErrorCode::IoFailure, "Unexpected end of file");
			}
			completed += static_cast<std::size_t>(read);
		}
		return {};
#else
		if (m_fileDescriptor < 0)
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}
		if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
		{
			return makeError(ErrorCode::FileTooLarge, "File offset does not fit into off_t");
		}

		std::size_t completed = 0;
		while (completed < destination.size())
		{
			const ssize_t read = pread(
				m_fileDescriptor,
				destination.data() + completed,
				destination.size() - completed,
				static_cast<off_t>(offset + completed));
			if (read < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				return makeError(ErrorCode::IoFailure, lastFileError("pread"));
			}
			if (read == 0)
			{
				return makeError(ErrorCode::IoFailure, "Unexpected end of file");
			}
			completed += static_cast<std::size_t>(read);
		}
		return {};
#endif
	}

	Result<void> NativeFile::writeAll(ByteSpan source)
	{
		if (source.empty())
		{
			return {};
		}

		std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
		if (!isValidHandle(m_handle))
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}

		std::size_t completed = 0;
		while (completed < source.size())
		{
			const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(source.size() - completed, 1024ull * 1024ull * 1024ull));
			DWORD written = 0;
			if (WriteFile(
				reinterpret_cast<HANDLE>(m_handle),
				source.data() + completed,
				requested,
				&written,
				nullptr) == 0)
			{
				return makeError(ErrorCode::IoFailure, lastFileError("WriteFile"));
			}
			if (written == 0)
			{
				return makeError(ErrorCode::IoFailure, "WriteFile wrote zero bytes");
			}
			completed += static_cast<std::size_t>(written);
		}
		return {};
#else
		if (m_fileDescriptor < 0)
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}

		std::size_t completed = 0;
		while (completed < source.size())
		{
			const ssize_t written = write(
				m_fileDescriptor,
				source.data() + completed,
				source.size() - completed);
			if (written < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				return makeError(ErrorCode::IoFailure, lastFileError("write"));
			}
			if (written == 0)
			{
				return makeError(ErrorCode::IoFailure, "write wrote zero bytes");
			}
			completed += static_cast<std::size_t>(written);
		}
		return {};
#endif
	}

	Result<void> NativeFile::flush()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
		if (!isValidHandle(m_handle))
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}
		if (FlushFileBuffers(reinterpret_cast<HANDLE>(m_handle)) == 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("FlushFileBuffers"));
		}
#else
		if (m_fileDescriptor < 0)
		{
			return makeError(ErrorCode::InvalidState, "NativeFile is not open");
		}
		if (fsync(m_fileDescriptor) != 0)
		{
			return makeError(ErrorCode::IoFailure, lastFileError("fsync"));
		}
#endif
		return {};
	}

	void NativeFile::close() noexcept
	{
		std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
		if (isValidHandle(m_handle))
		{
			CloseHandle(reinterpret_cast<HANDLE>(m_handle));
		}
		m_handle = nullptr;
#else
		if (m_fileDescriptor >= 0)
		{
			::close(m_fileDescriptor);
			m_fileDescriptor = -1;
		}
#endif
	}

	FileWriter::FileWriter(NativeFile file, std::pmr::memory_resource* memoryResource, std::size_t bufferSize)
		: m_file(std::move(file))
		, m_buffer(memoryResource, bufferSize == 0 ? 64ull * 1024ull : bufferSize, DefaultBufferAlignment)
	{
	}

	Result<void> FileWriter::write(ByteSpan data)
	{
		if (data.empty())
		{
			return {};
		}

		if (m_used + data.size() <= m_buffer.size())
		{
			std::memcpy(m_buffer.data() + m_used, data.data(), data.size());
			m_used += data.size();
			m_position += data.size();
			return {};
		}

		auto flushResult = flushBuffer();
		if (!flushResult)
		{
			return std::move(flushResult).error();
		}

		if (data.size() >= m_buffer.size())
		{
			auto writeResult = m_file.writeAll(data);
			if (!writeResult)
			{
				return std::move(writeResult).error();
			}
			m_position += data.size();
			return {};
		}

		std::memcpy(m_buffer.data(), data.data(), data.size());
		m_used = data.size();
		m_position += data.size();
		return {};
	}

	Result<void> FileWriter::flush(bool syncToDisk)
	{
		auto flushResult = flushBuffer();
		if (!flushResult)
		{
			return std::move(flushResult).error();
		}
		if (syncToDisk)
		{
			auto syncResult = m_file.flush();
			if (!syncResult)
			{
				return std::move(syncResult).error();
			}
		}
		return {};
	}

	std::uint64_t FileWriter::position() const noexcept
	{
		return m_position;
	}

	Result<void> FileWriter::flushBuffer()
	{
		if (m_used == 0)
		{
			return {};
		}

		auto writeResult = m_file.writeAll(ByteSpan(m_buffer.data(), m_used));
		if (!writeResult)
		{
			return std::move(writeResult).error();
		}
		m_used = 0;
		return {};
	}
}
