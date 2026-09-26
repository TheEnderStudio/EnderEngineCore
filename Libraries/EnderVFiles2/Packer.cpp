#include "EnderVFiles2.hpp"

#include <openssl/evp.h>

using namespace std;
using namespace std::filesystem;

EVF_NAMESPACE_BEGIN

namespace {
	struct FileNode {
		optional<Vector<FileNode*>> nextNodes;
		String            name;
		Path              path;
	};

	FileNode* buildFileTree(const Path& path) {
		FileNode* tree = new FileNode;
		if (is_regular_file(path)) {
			tree->nextNodes = nullopt;
			tree->name = path.filename().string();
			tree->path = path;
			return tree;
		}
		if (is_directory(path)) {
			tree->nextNodes = Vector<FileNode*>();
			for (const auto& entry : directory_iterator(path)) {
				tree->nextNodes->push_back(buildFileTree(entry.path()));
			}
			tree->name = path.filename().string();
			tree->path = path;
			return tree;
		}
		return nullptr;
	}

	void destroyFileTree(FileNode* tree) {
		if (!tree->nextNodes.has_value()) {
			delete tree;
		}
		for (auto& node : tree->nextNodes.value()) {
			destroyFileTree(node);
		}
	}

	struct VolumeEntry {
		UInt64       id;
		UInt64       pathSize; // EXCLUDE '\0'
		char*        path;
	};

	struct FileInIndex {
		UInt64       volumeId;
		UInt64       pathSize;
		char*        path;
	};

	struct IndexImage {
		UInt8        magicNumder[8];
		UInt64       passwordSHALength;
		UInt8        passwordSHA[EVP_MAX_MD_SIZE];
		UInt64       volumeCount;
		VolumeEntry* volumeEntries;
		UInt64       fileCount;
		FileInIndex* files;
	};

	struct VolumeHeader {
		UInt64       count;
		UInt64*      offsets;
	};

	struct DataEntry {
		UInt64       size;
		UInt8*       data;
	};

	struct VolumeImage {
		UInt8        magicNumder[8];
		VolumeHeader header;
		DataEntry*   data;
	};

	unique_ptr<IndexImage> serializeIndex(const Vector<UInt8>& data) {
		Size pos = 0;
		const Size headerSize = sizeof(IndexImage::magicNumder) + sizeof(IndexImage::passwordSHA) + sizeof(IndexImage::passwordSHALength) + sizeof(IndexImage::volumeCount);
		if (data.size() < headerSize) {
			return nullptr;
		}
		unique_ptr<IndexImage> image = make_unique<IndexImage>();
		memcpy(image.get(), data.data(), headerSize);
		pos += headerSize;
		for (UInt64 idx = 0; idx < image->volumeCount; idx++) {
			VolumeEntry* entry = new VolumeEntry;
			const Size volumeEntryHeaderSize = sizeof(VolumeEntry::id) + sizeof(VolumeEntry::pathSize);
			memcpy(entry, data.data() + pos, volumeEntryHeaderSize);
			char* pathBytes = new char[entry->pathSize];
		}
		return image;
	}
}

Packer& Packer::setAssetDir(const Path& path) {
	if (!exists(path)) {
		EVF_ERROR("ERROR: Path '%s' not exist", path.string().c_str());
		exit(1);
	}
	if (!is_directory(path)) {
		EVF_ERROR("ERROR: Path '%s' is not a directory", path.string().c_str());
		exit(1);
	}
	m_config.assetDir = path;
	return *this;
}

Packer& Packer::setOutputDir(const Path& path, bool replaceOld) {
	if (exists(path)) {
		if (!replaceOld) {
			EVF_ERROR("ERROR: Path '%s' exists. Call with 'replaceOld=true' to replace it", path.string().c_str());
			exit(1);
		}
		auto removedCount = remove_all(path);
		EVF_INFO("INFO: Removed %j items from '%s'", removedCount, path.string().c_str());
	}
	m_config.outputDir = path;
	return *this;
}

#define CheckWrite(path, bytes) if (!IOHelper::writeFile(path, bytes)) { EVF_ERROR("Failed to write file '%s'", path.string().c_str()); return; }

void Packer::pack() {
	Path indexPath = m_config.outputDir / (m_config.filename + ".index");
	if (!IOHelper::createFile(indexPath)) {
		EVF_ERROR("Cannot create file '%s', packing is terminated", indexPath.string().c_str());
		return;
	}
	CheckWrite(indexPath, EVF_WRAP_TO_VECTOR(MagicNumbers::IndexFileHeaderMagic));
	UInt64 volumeCount = 0;
	auto* tree = buildFileTree(m_config.assetDir);
}

EVF_NAMESPACE_END