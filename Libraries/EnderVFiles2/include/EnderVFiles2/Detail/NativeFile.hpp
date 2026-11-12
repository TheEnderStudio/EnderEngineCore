#pragma once

#include "Buffer.hpp"
#include "Error.hpp"
#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <mutex>
#include <span>

namespace EnderVFiles2::Detail
{
	class NativeFile
	{
	public:
		NativeFile() noexcept = default;
		NativeFile(NativeFile&& other) noexcept;
		NativeFile& operator=(NativeFile&& other) noexcept;
		NativeFile(const NativeFile&) = delete;
		NativeFile& operator=(const NativeFile&) = delete;
		~NativeFile();

		static Result<NativeFile> openRead(const std::filesystem::path& path);
		static Result<NativeFile> openWrite(const std::filesystem::path& path);

		bool isValid() const noexcept;
		Result<std::uint64_t> size() const;
		Result<void> readAt(std::uint64_t offset, MutableByteSpan destination) const;
		Result<void> writeAll(ByteSpan source);
		Result<void> flush();
		void close() noexcept;

	private:
#ifdef _WIN32
		void* m_handle = nullptr;
#else
		int m_fileDescriptor = -1;
#endif
		mutable std::mutex m_mutex;
	};

	class FileWriter
	{
	public:
		FileWriter(NativeFile file, std::pmr::memory_resource* memoryResource, std::size_t bufferSize);
		FileWriter(FileWriter&&) noexcept = default;
		FileWriter& operator=(FileWriter&&) noexcept = default;
		FileWriter(const FileWriter&) = delete;
		FileWriter& operator=(const FileWriter&) = delete;

		Result<void> write(ByteSpan data);
		Result<void> flush(bool syncToDisk);
		std::uint64_t position() const noexcept;

	private:
		Result<void> flushBuffer();

		NativeFile m_file;
		Buffer m_buffer;
		std::size_t m_used = 0;
		std::uint64_t m_position = 0;
	};
}
