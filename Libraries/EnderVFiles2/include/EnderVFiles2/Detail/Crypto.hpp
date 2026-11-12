#pragma once

#include "Error.hpp"
#include "Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace EnderVFiles2::Detail
{
	inline constexpr std::size_t Sha256DigestSize = 32;
	inline constexpr std::size_t AesGcmKeySize = 32;
	inline constexpr std::size_t AesGcmNonceSize = 12;
	inline constexpr std::size_t AesGcmTagSize = 16;
	inline constexpr std::size_t KdfSaltSize = 32;

	using Sha256Digest = std::array<Byte, Sha256DigestSize>;
	using AesGcmKey = std::array<Byte, AesGcmKeySize>;
	using AesGcmNonce = std::array<Byte, AesGcmNonceSize>;
	using AesGcmTag = std::array<Byte, AesGcmTagSize>;

	enum class CipherSuite : std::uint32_t
	{
		None = 0,
		Aes256Gcm = 1,
	};

	Result<void> randomBytes(MutableByteSpan destination);
	Result<void> sha256(ByteSpan data, Sha256Digest& digest);
	Result<void> pbkdf2Sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations, MutableByteSpan output);
	Result<void> hkdfSha256(ByteSpan inputKey, ByteSpan salt, ByteSpan info, MutableByteSpan output);
	bool constantTimeEqual(ByteSpan left, ByteSpan right) noexcept;
	void secureZero(MutableByteSpan bytes) noexcept;
	void secureClear(std::string& text) noexcept;
	std::string lastOpenSslError();

	class AesGcmEncryptor
	{
	public:
		AesGcmEncryptor() = default;
		AesGcmEncryptor(const AesGcmEncryptor&) = delete;
		AesGcmEncryptor& operator=(const AesGcmEncryptor&) = delete;
		~AesGcmEncryptor();

		Result<void> initialize(ByteSpan key, ByteSpan nonce, ByteSpan associatedData);
		Result<void> update(ByteSpan plaintext, MutableByteSpan ciphertext);
		Result<void> finalize(AesGcmTag& tag);

	private:
		void* m_context = nullptr;
		bool m_active = false;
		bool m_finalized = false;
	};

	class AesGcmDecryptor
	{
	public:
		AesGcmDecryptor() = default;
		AesGcmDecryptor(const AesGcmDecryptor&) = delete;
		AesGcmDecryptor& operator=(const AesGcmDecryptor&) = delete;
		~AesGcmDecryptor();

		Result<void> initialize(ByteSpan key, ByteSpan nonce, ByteSpan associatedData);
		Result<void> update(ByteSpan ciphertext, MutableByteSpan plaintext);
		Result<void> finalize(ByteSpan tag);

	private:
		void* m_context = nullptr;
		bool m_active = false;
		bool m_finalized = false;
	};
}
