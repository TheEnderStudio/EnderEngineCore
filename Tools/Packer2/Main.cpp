#include <EnderVFiles2/EnderVFiles2.hpp>

#include <cxxopts.hpp>

#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
	constexpr std::size_t MaximumPasswordBytes = 4096;

	std::string_view trimView(std::string_view text) noexcept
	{
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
		{
			text.remove_prefix(1);
		}
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
		{
			text.remove_suffix(1);
		}
		return text;
	}

	bool equalsIgnoreCase(std::string_view left, std::string_view right) noexcept
	{
		if (left.size() != right.size())
		{
			return false;
		}
		for (std::size_t index = 0; index < left.size(); ++index)
		{
			const unsigned char leftCharacter = static_cast<unsigned char>(left[index]);
			const unsigned char rightCharacter = static_cast<unsigned char>(right[index]);
			if (std::tolower(leftCharacter) != std::tolower(rightCharacter))
			{
				return false;
			}
		}
		return true;
	}

	bool parseByteSize(std::string_view text, std::uint64_t& value) noexcept
	{
		text = trimView(text);
		if (text.empty() || text.front() == '-')
		{
			return false;
		}

		std::uint64_t number = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), number, 10);
		if (result.ec != std::errc() || result.ptr == text.data())
		{
			return false;
		}

		const std::string_view suffix = trimView(std::string_view(result.ptr, static_cast<std::size_t>(text.data() + text.size() - result.ptr)));
		std::uint64_t multiplier = 1;
		if (suffix.empty() || equalsIgnoreCase(suffix, "b"))
		{
			multiplier = 1;
		}
		else if (equalsIgnoreCase(suffix, "k") || equalsIgnoreCase(suffix, "kb") || equalsIgnoreCase(suffix, "kib"))
		{
			multiplier = 1024ull;
		}
		else if (equalsIgnoreCase(suffix, "m") || equalsIgnoreCase(suffix, "mb") || equalsIgnoreCase(suffix, "mib"))
		{
			multiplier = 1024ull * 1024ull;
		}
		else if (equalsIgnoreCase(suffix, "g") || equalsIgnoreCase(suffix, "gb") || equalsIgnoreCase(suffix, "gib"))
		{
			multiplier = 1024ull * 1024ull * 1024ull;
		}
		else if (equalsIgnoreCase(suffix, "t") || equalsIgnoreCase(suffix, "tb") || equalsIgnoreCase(suffix, "tib"))
		{
			multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
		}
		else
		{
			return false;
		}

		if (number > std::numeric_limits<std::uint64_t>::max() / multiplier)
		{
			return false;
		}
		value = number * multiplier;
		return true;
	}

	bool parseUint32(std::string_view text, std::uint32_t& value) noexcept
	{
		text = trimView(text);
		if (text.empty())
		{
			return false;
		}

		std::uint32_t parsed = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
		if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		{
			return false;
		}
		value = parsed;
		return true;
	}

	bool parseCompressionLevel(std::string_view text, EnderVFiles2::CompressionLevel& level) noexcept
	{
		text = trimView(text);
		if (equalsIgnoreCase(text, "store") || equalsIgnoreCase(text, "none") || text == "0")
		{
			level = EnderVFiles2::CompressionLevel::Store;
			return true;
		}
		if (equalsIgnoreCase(text, "fast") || equalsIgnoreCase(text, "fastest") || text == "1")
		{
			level = EnderVFiles2::CompressionLevel::Fastest;
			return true;
		}
		if (equalsIgnoreCase(text, "default") || equalsIgnoreCase(text, "normal") || text == "6")
		{
			level = EnderVFiles2::CompressionLevel::Default;
			return true;
		}
		if (equalsIgnoreCase(text, "best") || equalsIgnoreCase(text, "max") || text == "9")
		{
			level = EnderVFiles2::CompressionLevel::Best;
			return true;
		}
		return false;
	}

	bool readPasswordFile(const std::filesystem::path& path, std::string& password, std::string& error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			error = "failed to open password file";
			return false;
		}

		std::string content(MaximumPasswordBytes + 1, '\0');
		file.read(content.data(), static_cast<std::streamsize>(content.size()));
		const std::streamsize readBytes = file.gcount();
		if (file.bad())
		{
			error = "failed to read password file";
			return false;
		}
		content.resize(static_cast<std::size_t>(readBytes));

		while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
		{
			content.pop_back();
		}
		if (content.empty())
		{
			error = "password file is empty";
			return false;
		}
		if (content.size() > MaximumPasswordBytes)
		{
			error = "password file exceeds 4096 bytes";
			return false;
		}

		password = std::move(content);
		return true;
	}

	bool readPasswordFromStdin(std::string& password, std::string& error)
	{
		if (!std::getline(std::cin, password))
		{
			error = "failed to read password from stdin";
			return false;
		}
		if (!password.empty() && password.back() == '\r')
		{
			password.pop_back();
		}
		if (password.empty())
		{
			error = "password from stdin is empty";
			return false;
		}
		if (password.size() > MaximumPasswordBytes)
		{
			error = "password from stdin exceeds 4096 bytes";
			return false;
		}
		return true;
	}

	void printError(const EnderVFiles2::Error& error)
	{
		std::fprintf(stderr, "Packer2: error %u: %s\n",
			static_cast<unsigned int>(error.code),
			error.message.c_str());
	}

	int usageError(const std::string& message)
	{
		std::fprintf(stderr, "Packer2: %s\n", message.c_str());
		return 2;
	}

	int run(int argc, char** argv)
	{
		cxxopts::Options options("Packer2", "Build EnderVFiles2 packages from a directory tree");
		options.add_options()
			("i,input", "Input asset directory", cxxopts::value<std::string>())
			("o,output", "Output package directory", cxxopts::value<std::string>())
			("n,name", "Package name", cxxopts::value<std::string>())
			("p,password", "Password (visible in the process command line)", cxxopts::value<std::string>())
			("password-file", "Read the password from a file", cxxopts::value<std::string>())
			("password-stdin", "Read the password from stdin", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
			("s,volume-size", "Target volume size (K/KiB/M/MiB/G/GiB, 0 = single volume)", cxxopts::value<std::string>()->default_value("1G"))
			("compression", "Compression: store, fastest, default, best", cxxopts::value<std::string>()->default_value("default"))
			("compression-threshold", "Maximum file size to attempt compression", cxxopts::value<std::string>()->default_value("64M"))
			("kdf-iterations", "PBKDF2 iteration count", cxxopts::value<std::string>()->default_value("300000"))
			("io-buffer-size", "I/O buffer size", cxxopts::value<std::string>()->default_value("1M"))
			("alignment", "Buffer alignment", cxxopts::value<std::string>()->default_value("64"))
			("f,overwrite", "Overwrite existing output package files", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
			("q,quiet", "Suppress the success summary", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
			("version", "Print the tool version")
			("h,help", "Print this help");

		const cxxopts::ParseResult result = options.parse(argc, argv);

		if (result.count("help") != 0)
		{
			const std::string helpText = options.help();
			std::fputs(helpText.c_str(), stdout);
			return 0;
		}
		if (result.count("version") != 0)
		{
			std::printf("Packer2 1.0.0 (EnderVFiles2)\n");
			return 0;
		}

		for (const char* required : { "input", "output", "name" })
		{
			if (result.count(required) == 0)
			{
				return usageError(std::string("missing required option --") + required);
			}
		}

		const std::string inputText = result["input"].as<std::string>();
		const std::string outputText = result["output"].as<std::string>();
		const std::string packageName = result["name"].as<std::string>();
		if (inputText.empty())
		{
			return usageError("--input must not be empty");
		}
		if (outputText.empty())
		{
			return usageError("--output must not be empty");
		}
		if (packageName.empty())
		{
			return usageError("--name must not be empty");
		}

		std::uint64_t volumeSize = 0;
		if (!parseByteSize(result["volume-size"].as<std::string>(), volumeSize))
		{
			return usageError("invalid --volume-size");
		}

		std::uint64_t compressionThreshold = 0;
		if (!parseByteSize(result["compression-threshold"].as<std::string>(), compressionThreshold))
		{
			return usageError("invalid --compression-threshold");
		}

		std::uint32_t kdfIterations = 0;
		if (!parseUint32(result["kdf-iterations"].as<std::string>(), kdfIterations))
		{
			return usageError("invalid --kdf-iterations");
		}

		std::uint64_t ioBufferSize = 0;
		if (!parseByteSize(result["io-buffer-size"].as<std::string>(), ioBufferSize)
			|| ioBufferSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		{
			return usageError("invalid --io-buffer-size");
		}

		std::uint64_t alignment = 0;
		if (!parseByteSize(result["alignment"].as<std::string>(), alignment)
			|| alignment > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		{
			return usageError("invalid --alignment");
		}

		EnderVFiles2::CompressionLevel compressionLevel = EnderVFiles2::CompressionLevel::Default;
		if (!parseCompressionLevel(result["compression"].as<std::string>(), compressionLevel))
		{
			return usageError("invalid --compression (expected store, fastest, default, or best)");
		}

		const bool passwordFromStdin = result["password-stdin"].as<bool>();
		const std::size_t passwordSources =
			result.count("password")
			+ result.count("password-file")
			+ (passwordFromStdin ? 1u : 0u);
		if (passwordSources > 1)
		{
			return usageError("use only one of --password, --password-file, or --password-stdin");
		}

		std::string password;
		if (result.count("password") != 0)
		{
			password = result["password"].as<std::string>();
			if (password.size() > MaximumPasswordBytes)
			{
				return usageError("--password exceeds 4096 bytes");
			}
		}
		else if (result.count("password-file") != 0)
		{
			const std::string passwordFilePath = result["password-file"].as<std::string>();
			if (passwordFilePath.empty())
			{
				return usageError("--password-file must not be empty");
			}
			std::string error;
			if (!readPasswordFile(std::filesystem::path(passwordFilePath), password, error))
			{
				return usageError(error);
			}
		}
		else if (passwordFromStdin)
		{
			std::string error;
			if (!readPasswordFromStdin(password, error))
			{
				return usageError(error);
			}
		}

		EnderVFiles2::PackerConfig config;
		config.inputDirectory = std::filesystem::path(inputText);
		config.outputDirectory = std::filesystem::path(outputText);
		config.packageName = packageName;
		config.password = std::move(password);
		config.volumeSize = volumeSize;
		config.compressionThresholdBytes = compressionThreshold;
		config.kdfIterations = kdfIterations;
		config.ioBufferSize = static_cast<std::size_t>(ioBufferSize);
		config.bufferAlignment = static_cast<std::size_t>(alignment);
		config.compressionLevel = compressionLevel;
		config.overwriteExisting = result["overwrite"].as<bool>();

		EnderVFiles2::Packer packer(std::move(config));
		auto packResult = packer.pack();
		if (!packResult)
		{
			printError(packResult.error());
			return 1;
		}

		const EnderVFiles2::PackResult& summary = packResult.value();
		if (!result["quiet"].as<bool>())
		{
			std::printf("Package : %s\n", packageName.c_str());
			std::printf("Index   : %s\n", summary.indexPath.string().c_str());
			std::printf("Volumes : %u\n", static_cast<unsigned int>(summary.volumeCount));
			std::printf("Files   : %u\n", static_cast<unsigned int>(summary.fileCount));
			std::printf("Dirs    : %u\n", static_cast<unsigned int>(summary.directoryCount));
			std::printf("Skipped : %u\n", static_cast<unsigned int>(summary.skippedEntries));
			std::printf("Input   : %llu bytes\n", static_cast<unsigned long long>(summary.totalUncompressedBytes));
			std::printf("Stored  : %llu bytes\n", static_cast<unsigned long long>(summary.totalStoredBytes));
			std::fputc('\n', stdout);
		}
		return 0;
	}
}

int main(int argc, char** argv)
{
	return run(argc, argv);
}
