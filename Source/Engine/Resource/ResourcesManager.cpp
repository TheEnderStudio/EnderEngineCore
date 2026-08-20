#include <Resource/ResourcesManager.hpp>
#include <Core/Log.hpp>

#include <EnderVFiles.hpp>
#include <filesystem>

EE_NAMESPACE_BEGIN

struct ResourcesManager::Impl {
	EnderVirtualFileSystem* vfs = nullptr;
	bool mounted = false;

	~Impl() { if (vfs) { delete vfs; vfs = nullptr; } }
};

ResourcesManager::ResourcesManager() : m_impl(std::make_unique<Impl>()) {}
ResourcesManager::~ResourcesManager() {}

ResourcesManager& ResourcesManager::getInstance() {
	static ResourcesManager instance;
	return instance;
}

Result<void, ResourceError> ResourcesManager::mount(const Path& indexPath, const String& password) {
	auto& p = *m_impl;
	if (p.mounted) { unmount(); }

	if (!std::filesystem::exists(indexPath)) {
		EError("Resource: index not found: {}", indexPath.string());
		return ResourceError::FileNotFound;
	}

	auto key = EnderGenerateKey(password);
	EnderCacheConfig cc; cc.m_maxSize = 256 * 1024 * 1024; cc.m_maxEntries = 4096;

	auto* vfs = new EnderVirtualFileSystem();
	if (!vfs->initialize(indexPath.string(), key, cc)) {
		EError("Resource: mount failed for {}", indexPath.string());
		delete vfs;
		return ResourceError::MountFailed;
	}

	p.vfs = vfs;
	p.mounted = true;
	EInfo("Resource: mounted {}", indexPath.string());
	return {};
}

void ResourcesManager::unmount() {
	auto& p = *m_impl;
	if (!p.mounted) return;
	delete p.vfs; p.vfs = nullptr;
	p.mounted = false;
}

bool ResourcesManager::isMounted() const { return m_impl->mounted; }

Result<ResData, ResourceError> ResourcesManager::readFile(const ResPath& virtualPath) {
	auto& p = *m_impl;
	if (!p.mounted) return ResourceError::NotInitialized;

	std::string vp = virtualPath.path.string();
	if (vp.starts_with('/')) {
		vp = vp.substr(1);
	}
	auto data = p.vfs->readFile(vp);
	if (data.empty()) {
		if (!p.vfs->fileExists(vp)) return ResourceError::FileNotFound;
		return ResourceError::CorruptedData;
	}
	return data;
}

void ResourcesManager::readFileAsync(const ResPath& virtualPath, ResourceAsyncCallback callback) {
	auto& p = *m_impl;
	if (!p.mounted) {
		if (callback) callback(virtualPath, {}, ResourceError::NotInitialized);
		return;
	}
	std::string vp = virtualPath.path.string();
	p.vfs->readFileAsync(vp, [cb = std::move(callback), rp = virtualPath](const std::string&, std::vector<uint8_t> data, EnderErrorCode ec) {
		if (!cb) return;
		ResourceError re;
		switch (ec) {
		case EnderErrorCode::Success:         re = ResourceError::None; break;
		case EnderErrorCode::FileNotFound:    re = ResourceError::FileNotFound; break;
		case EnderErrorCode::InvalidPassword: re = ResourceError::InvalidPassword; break;
		case EnderErrorCode::CorruptedData:   re = ResourceError::CorruptedData; break;
		case EnderErrorCode::VolumeNotFound:  re = ResourceError::VolumeNotFound; break;
		case EnderErrorCode::OutOfMemory:     re = ResourceError::OutOfMemory; break;
		default:                              re = ResourceError::MountFailed; break;
		}
		cb(rp, data, re);
	});
}

bool ResourcesManager::fileExists(const ResPath& virtualPath) {
	if (!m_impl->mounted) return false;
	return m_impl->vfs->fileExists(virtualPath.path.string());
}

Vector<ResPath> ResourcesManager::listFiles(const ResPath& prefix) {
	auto& p = *m_impl;
	if (!p.mounted) return {};
	auto raw = p.vfs->listFiles(prefix.path.string());
	Vector<ResPath> out; out.reserve(raw.size());
	for (auto& s : raw) out.push_back(ResPath(Path(s)));
	return out;
}

void ResourcesManager::setCacheSize(Size maxSize) {
	if (m_impl->mounted) m_impl->vfs->setCacheSize(maxSize);
}
void ResourcesManager::clearCache() {
	if (m_impl->mounted) m_impl->vfs->clearCache();
}
Size ResourcesManager::cacheHitCount() const {
	return m_impl->mounted ? m_impl->vfs->getCacheHitCount() : 0;
}
Size ResourcesManager::cacheMissCount() const {
	return m_impl->mounted ? m_impl->vfs->getCacheMissCount() : 0;
}

EE_NAMESPACE_END
