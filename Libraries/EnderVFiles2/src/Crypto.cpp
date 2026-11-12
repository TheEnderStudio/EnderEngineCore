#include "Detail/Crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <limits>
#include <string>

namespace EnderVFiles2::Detail
{
	namespace
	{
		constexpr std::size_t Sha256BlockSize = 64;

		EVP_CIPHER_CTX* asContext(void* context) noexcept
		{
			return reinterpret_cast<EVP_CIPHER_CTX*>(context);
		}

		void* asOpaqueContext(EVP_CIPHER_CTX* context) noexcept
		{
			return context;
		}

		Result<void> digestSha256(ByteSpan first, ByteSpan second, ByteSpan third, Sha256Digest& digest)
		{
			EVP_MD_CTX* context = EVP_MD_CTX_new();
			if (context == nullptr)
			{
				return makeError(ErrorCode::CryptoFailure, "EVP_MD_CTX_new failed");
			}

			bool success = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
			if (success && !first.empty())
			{
				success = EVP_DigestUpdate(context, first.data(), first.size()) == 1;
			}
			if (success && !second.empty())
			{
				success = EVP_DigestUpdate(context, second.data(), second.size()) == 1;
			}
			if (success && !third.empty())
			{
				success = EVP_DigestUpdate(context, third.data(), third.size()) == 1;
			}

			unsigned int digestLength = 0;
			if (success)
			{
				success = EVP_DigestFinal_ex(context, digest.data(), &digestLength) == 1 && digestLength == digest.size();
			}

			EVP_MD_CTX_free(context);

			if (!success)
			{
				return makeError(ErrorCode::CryptoFailure, "SHA-256 computation failed: " + lastOpenSslError());
			}
			return {};
		}

		Result<void> hmacSha256(ByteSpan key, ByteSpan first, ByteSpan second, ByteSpan third, Sha256Digest& digest)
		{
			std::array<Byte, Sha256BlockSize> keyBlock{};
			if (key.size() > Sha256BlockSize)
			{
				Sha256Digest keyDigest{};
				auto digestResult = sha256(key, keyDigest);
				if (!digestResult)
				{
					return std::move(digestResult).error();
				}
				std::memcpy(keyBlock.data(), keyDigest.data(), keyDigest.size());
				OPENSSL_cleanse(keyDigest.data(), keyDigest.size());
			}
			else if (!key.empty())
			{
				std::memcpy(keyBlock.data(), key.data(), key.size());
			}

			std::array<Byte, Sha256BlockSize> innerPad{};
			std::array<Byte, Sha256BlockSize> outerPad{};
			for (std::size_t index = 0; index < Sha256BlockSize; ++index)
			{
				innerPad[index] = static_cast<Byte>(keyBlock[index] ^ 0x36);
				outerPad[index] = static_cast<Byte>(keyBlock[index] ^ 0x5C);
			}

			EVP_MD_CTX* context = EVP_MD_CTX_new();
			if (context == nullptr)
			{
				OPENSSL_cleanse(keyBlock.data(), keyBlock.size());
				OPENSSL_cleanse(innerPad.data(), innerPad.size());
				OPENSSL_cleanse(outerPad.data(), outerPad.size());
				return makeError(ErrorCode::CryptoFailure, "EVP_MD_CTX_new failed");
			}

			bool success = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
			if (success)
			{
				success = EVP_DigestUpdate(context, innerPad.data(), innerPad.size()) == 1;
			}
			if (success && !first.empty())
			{
				success = EVP_DigestUpdate(context, first.data(), first.size()) == 1;
			}
			if (success && !second.empty())
			{
				success = EVP_DigestUpdate(context, second.data(), second.size()) == 1;
			}
			if (success && !third.empty())
			{
				success = EVP_DigestUpdate(context, third.data(), third.size()) == 1;
			}

			Sha256Digest innerDigest{};
			unsigned int innerLength = 0;
			if (success)
			{
				success = EVP_DigestFinal_ex(context, innerDigest.data(), &innerLength) == 1 && innerLength == innerDigest.size();
			}
			EVP_MD_CTX_free(context);

			EVP_MD_CTX* outerContext = success ? EVP_MD_CTX_new() : nullptr;
			bool outerSuccess = outerContext != nullptr;
			if (outerSuccess)
			{
				outerSuccess = EVP_DigestInit_ex(outerContext, EVP_sha256(), nullptr) == 1;
			}
			if (outerSuccess)
			{
				outerSuccess = EVP_DigestUpdate(outerContext, outerPad.data(), outerPad.size()) == 1;
			}
			if (outerSuccess)
			{
				outerSuccess = EVP_DigestUpdate(outerContext, innerDigest.data(), innerDigest.size()) == 1;
			}

			unsigned int digestLength = 0;
			if (outerSuccess)
			{
				outerSuccess = EVP_DigestFinal_ex(outerContext, digest.data(), &digestLength) == 1 && digestLength == digest.size();
			}
			if (outerContext != nullptr)
			{
				EVP_MD_CTX_free(outerContext);
			}

			OPENSSL_cleanse(keyBlock.data(), keyBlock.size());
			OPENSSL_cleanse(innerPad.data(), innerPad.size());
			OPENSSL_cleanse(outerPad.data(), outerPad.size());
			OPENSSL_cleanse(innerDigest.data(), innerDigest.size());

			if (!success || !outerSuccess)
			{
				return makeError(ErrorCode::CryptoFailure, "HMAC-SHA256 failed: " + lastOpenSslError());
			}
			return {};
		}
	}

	std::string lastOpenSslError()
	{
		const unsigned long code = ERR_get_error();
		if (code == 0)
		{
			return "OpenSSL error queue is empty";
		}

		char buffer[256] = {};
		ERR_error_string_n(code, buffer, sizeof(buffer));
		ERR_clear_error();
		return std::string(buffer);
	}

	Result<void> randomBytes(MutableByteSpan destination)
	{
		std::size_t offset = 0;
		while (offset < destination.size())
		{
			const std::size_t remaining = destination.size() - offset;
			const int chunk = static_cast<int>(std::min<std::size_t>(remaining, static_cast<std::size_t>(INT_MAX)));
			if (RAND_bytes(destination.data() + offset, chunk) != 1)
			{
				return makeError(ErrorCode::RandomFailure, "RAND_bytes failed: " + lastOpenSslError());
			}
			offset += static_cast<std::size_t>(chunk);
		}
		return {};
	}

	Result<void> sha256(ByteSpan data, Sha256Digest& digest)
	{
		return digestSha256(data, {}, {}, digest);
	}

	Result<void> pbkdf2Sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations, MutableByteSpan output)
	{
		if (iterations == 0 || output.empty())
		{
			return makeError(ErrorCode::InvalidArgument, "PBKDF2 requires a positive iteration count and a non-empty output");
		}
		if (password.size() > static_cast<std::size_t>(INT_MAX)
			|| salt.size() > static_cast<std::size_t>(INT_MAX)
			|| output.size() > static_cast<std::size_t>(INT_MAX))
		{
			return makeError(ErrorCode::InvalidArgument, "PBKDF2 input is too large");
		}

		const char* passwordData = password.empty() ? "" : password.data();
		const unsigned char* saltData = salt.empty() ? reinterpret_cast<const unsigned char*>("") : salt.data();
		const int result = PKCS5_PBKDF2_HMAC(
			passwordData,
			static_cast<int>(password.size()),
			saltData,
			static_cast<int>(salt.size()),
			static_cast<int>(iterations),
			EVP_sha256(),
			static_cast<int>(output.size()),
			output.data());

		if (result != 1)
		{
			return makeError(ErrorCode::CryptoFailure, "PBKDF2-HMAC-SHA256 failed: " + lastOpenSslError());
		}
		return {};
	}

	Result<void> hkdfSha256(ByteSpan inputKey, ByteSpan salt, ByteSpan info, MutableByteSpan output)
	{
		if (output.empty() || output.size() > 255ull * Sha256DigestSize)
		{
			return makeError(ErrorCode::InvalidArgument, "HKDF output length is invalid");
		}

		Sha256Digest pseudoRandomKey{};
		auto extractResult = hmacSha256(salt, inputKey, {}, {}, pseudoRandomKey);
		if (!extractResult)
		{
			return std::move(extractResult).error();
		}

		Sha256Digest previous{};
		std::size_t offset = 0;
		Byte counter = 1;
		bool firstBlock = true;
		while (offset < output.size())
		{
			const Byte counterByte = counter;
			Sha256Digest block{};
			const ByteSpan previousSpan = firstBlock
				? ByteSpan()
				: ByteSpan(previous.data(), previous.size());
			auto expandResult = hmacSha256(pseudoRandomKey, previousSpan, info, ByteSpan(&counterByte, 1), block);
			if (!expandResult)
			{
				OPENSSL_cleanse(pseudoRandomKey.data(), pseudoRandomKey.size());
				OPENSSL_cleanse(previous.data(), previous.size());
				return std::move(expandResult).error();
			}

			const std::size_t copySize = std::min(output.size() - offset, block.size());
			std::memcpy(output.data() + offset, block.data(), copySize);
			previous = block;
			OPENSSL_cleanse(block.data(), block.size());
			offset += copySize;
			firstBlock = false;
			if (counter == 255 && offset < output.size())
			{
				OPENSSL_cleanse(pseudoRandomKey.data(), pseudoRandomKey.size());
				OPENSSL_cleanse(previous.data(), previous.size());
				return makeError(ErrorCode::InvalidArgument, "HKDF output is too long");
			}
			++counter;
		}

		OPENSSL_cleanse(pseudoRandomKey.data(), pseudoRandomKey.size());
		OPENSSL_cleanse(previous.data(), previous.size());
		return {};
	}

	bool constantTimeEqual(ByteSpan left, ByteSpan right) noexcept
	{
		if (left.size() != right.size())
		{
			return false;
		}
		if (left.empty())
		{
			return true;
		}
		return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
	}

	void secureZero(MutableByteSpan bytes) noexcept
	{
		if (!bytes.empty())
		{
			OPENSSL_cleanse(bytes.data(), bytes.size());
		}
	}

	void secureClear(std::string& text) noexcept
	{
		if (!text.empty())
		{
			OPENSSL_cleanse(text.data(), text.size());
		}
		text.clear();
	}

	AesGcmEncryptor::~AesGcmEncryptor()
	{
		if (m_context != nullptr)
		{
			EVP_CIPHER_CTX_free(asContext(m_context));
			m_context = nullptr;
		}
	}

	Result<void> AesGcmEncryptor::initialize(ByteSpan key, ByteSpan nonce, ByteSpan associatedData)
	{
		if (m_context != nullptr || m_active || m_finalized)
		{
			return makeError(ErrorCode::InvalidState, "AES-GCM encryptor is already initialized");
		}
		if (key.size() != AesGcmKeySize || nonce.size() != AesGcmNonceSize)
		{
			return makeError(ErrorCode::InvalidArgument, "AES-256-GCM requires a 32-byte key and a 12-byte nonce");
		}
		if (associatedData.size() > static_cast<std::size_t>(INT_MAX))
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM associated data is too large");
		}

		ERR_clear_error();
		EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
		if (context == nullptr)
		{
			return makeError(ErrorCode::CryptoFailure, "EVP_CIPHER_CTX_new failed");
		}
		m_context = asOpaqueContext(context);

		bool success = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
		if (success)
		{
			success = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(AesGcmNonceSize), nullptr) == 1;
		}
		if (success)
		{
			success = EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1;
		}
		if (success && !associatedData.empty())
		{
			int outputLength = 0;
			success = EVP_EncryptUpdate(context, nullptr, &outputLength, associatedData.data(), static_cast<int>(associatedData.size())) == 1;
		}

		if (!success)
		{
			const std::string message = lastOpenSslError();
			EVP_CIPHER_CTX_free(context);
			m_context = nullptr;
			return makeError(ErrorCode::CryptoFailure, "AES-GCM initialization failed: " + message);
		}

		m_active = true;
		return {};
	}

	Result<void> AesGcmEncryptor::update(ByteSpan plaintext, MutableByteSpan ciphertext)
	{
		if (!m_active || m_finalized)
		{
			return makeError(ErrorCode::InvalidState, "AES-GCM encryptor is not active");
		}
		if (plaintext.size() != ciphertext.size())
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM input and output sizes differ");
		}
		if (plaintext.empty())
		{
			return {};
		}
		if (plaintext.size() > static_cast<std::size_t>(INT_MAX))
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM update is too large");
		}

		int outputLength = 0;
		if (EVP_EncryptUpdate(asContext(m_context), ciphertext.data(), &outputLength, plaintext.data(), static_cast<int>(plaintext.size())) != 1)
		{
			return makeError(ErrorCode::CryptoFailure, "AES-GCM encryption failed: " + lastOpenSslError());
		}
		if (outputLength != static_cast<int>(plaintext.size()))
		{
			return makeError(ErrorCode::CryptoFailure, "AES-GCM produced an unexpected output length");
		}
		return {};
	}

	Result<void> AesGcmEncryptor::finalize(AesGcmTag& tag)
	{
		if (!m_active || m_finalized)
		{
			return makeError(ErrorCode::InvalidState, "AES-GCM encryptor is not active");
		}

		std::array<Byte, 16> finalBlock{};
		int outputLength = 0;
		if (EVP_EncryptFinal_ex(asContext(m_context), finalBlock.data(), &outputLength) != 1)
		{
			return makeError(ErrorCode::CryptoFailure, "AES-GCM finalization failed: " + lastOpenSslError());
		}
		if (outputLength != 0)
		{
			return makeError(ErrorCode::CryptoFailure, "AES-GCM produced trailing data");
		}
		if (EVP_CIPHER_CTX_ctrl(asContext(m_context), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) != 1)
		{
			return makeError(ErrorCode::CryptoFailure, "AES-GCM tag extraction failed: " + lastOpenSslError());
		}

		EVP_CIPHER_CTX_free(asContext(m_context));
		m_context = nullptr;
		m_active = false;
		m_finalized = true;
		return {};
	}

	AesGcmDecryptor::~AesGcmDecryptor()
	{
		if (m_context != nullptr)
		{
			EVP_CIPHER_CTX_free(asContext(m_context));
			m_context = nullptr;
		}
	}

	Result<void> AesGcmDecryptor::initialize(ByteSpan key, ByteSpan nonce, ByteSpan associatedData)
	{
		if (m_context != nullptr || m_active || m_finalized)
		{
			return makeError(ErrorCode::InvalidState, "AES-GCM decryptor is already initialized");
		}
		if (key.size() != AesGcmKeySize || nonce.size() != AesGcmNonceSize)
		{
			return makeError(ErrorCode::InvalidArgument, "AES-256-GCM requires a 32-byte key and a 12-byte nonce");
		}
		if (associatedData.size() > static_cast<std::size_t>(INT_MAX))
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM associated data is too large");
		}

		ERR_clear_error();
		EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
		if (context == nullptr)
		{
			return makeError(ErrorCode::CryptoFailure, "EVP_CIPHER_CTX_new failed");
		}
		m_context = asOpaqueContext(context);

		bool success = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
		if (success)
		{
			success = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(AesGcmNonceSize), nullptr) == 1;
		}
		if (success)
		{
			success = EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1;
		}
		if (success && !associatedData.empty())
		{
			int outputLength = 0;
			success = EVP_DecryptUpdate(context, nullptr, &outputLength, associatedData.data(), static_cast<int>(associatedData.size())) == 1;
		}

		if (!success)
		{
			const std::string message = lastOpenSslError();
			EVP_CIPHER_CTX_free(context);
			m_context = nullptr;
			return makeError(ErrorCode::CryptoFailure, "AES-GCM initialization failed: " + message);
		}

		m_active = true;
		return {};
	}

	Result<void> AesGcmDecryptor::update(ByteSpan ciphertext, MutableByteSpan plaintext)
	{
		if (!m_active || m_finalized)
		{
			return makeError(ErrorCode::InvalidState, "AES-GCM decryptor is not active");
		}
		if (ciphertext.size() != plaintext.size())
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM input and output sizes differ");
		}
		if (ciphertext.empty())
		{
			return {};
		}
		if (ciphertext.size() > static_cast<std::size_t>(INT_MAX))
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM update is too large");
		}

		int outputLength = 0;
		if (EVP_DecryptUpdate(asContext(m_context), plaintext.data(), &outputLength, ciphertext.data(), static_cast<int>(ciphertext.size())) != 1)
		{
			return makeError(ErrorCode::DecryptionFailed, "AES-GCM decryption failed: " + lastOpenSslError());
		}
		if (outputLength != static_cast<int>(ciphertext.size()))
		{
			return makeError(ErrorCode::DecryptionFailed, "AES-GCM produced an unexpected output length");
		}
		return {};
	}

	Result<void> AesGcmDecryptor::finalize(ByteSpan tag)
	{
		if (!m_active || m_finalized)
		{
			return makeError(ErrorCode::InvalidState, "AES-GCM decryptor is not active");
		}
		if (tag.size() != AesGcmTagSize)
		{
			return makeError(ErrorCode::InvalidArgument, "AES-GCM tag has an invalid size");
		}

		if (EVP_CIPHER_CTX_ctrl(asContext(m_context), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), const_cast<Byte*>(tag.data())) != 1)
		{
			return makeError(ErrorCode::CryptoFailure, "AES-GCM tag setup failed: " + lastOpenSslError());
		}

		std::array<Byte, 16> finalBlock{};
		int outputLength = 0;
		const int result = EVP_DecryptFinal_ex(asContext(m_context), finalBlock.data(), &outputLength);
		const std::string message = result == 1 ? std::string() : lastOpenSslError();

		EVP_CIPHER_CTX_free(asContext(m_context));
		m_context = nullptr;
		m_active = false;
		m_finalized = true;

		if (result != 1 || outputLength != 0)
		{
			return makeError(ErrorCode::AuthenticationFailed,
				message.empty() ? "AES-GCM authentication failed" : "AES-GCM authentication failed: " + message);
		}
		return {};
	}
}
