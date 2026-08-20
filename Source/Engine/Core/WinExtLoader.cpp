#include <Core/Extension.hpp>
#include <Core/Log.hpp>
#include <Core/ExtensionAPI.h>

#include <filesystem>

#ifdef EE_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace fs = std::filesystem;

using Path = fs::path;

EE_NAMESPACE_BEGIN

namespace Detail {
	bool initialized = false;
	HashMap<String, Path> nameToDll{};
	HashMap<String, Extension*> loadedExts{};
}

void ExtensionLoader::initialize() {
	if (Detail::initialized) {
		EWarn("ExtensionLoader::initialize() called but the loader is already initialized.");
		return;
	}

	Vector<Path> files;
	for (const auto& entry : fs::directory_iterator(fs::current_path())) {
		if (fs::is_regular_file(entry.status()) && entry.path().extension().string() == ".dll") {
			files.push_back(entry.path().filename());
		}
	}

	Detail::nameToDll.clear();
	Detail::loadedExts.clear();

	for (const auto& f : files) {
		HMODULE hDll = LoadLibraryA(f.string().c_str());
		if (!hDll)
			continue;
		auto pApi = (eeExtGetExtensionPtrFuncPtr)GetProcAddress(hDll, GetExtPtrFuncNameStr);
		if (!pApi) {
			pApi = (eeExtGetExtensionPtrFuncPtr)GetProcAddress(hDll, ("_" + String(GetExtPtrFuncNameStr)).c_str());
		}
		if (!pApi) {
			FreeLibrary(hDll);
			continue;
		}
		Extension* ext = (Extension*)pApi();
		if (ext->magicNum != ExtensionMagicNumber) {
			EDebug("[Preload] The magic number '{}' is mismatched! (should be '{}')", ext->magicNum, ExtensionMagicNumber);
			goto Free1;
		}
		if (auto ver = ext->getApiVersion(); ver > CurrentExtensionApiVersion) {
			EDebug("[Preload] Unsupported API Version: '{}' (current: '{}')", ver, CurrentExtensionApiVersion);
			goto Free1;
		}
		goto Insert;
	Free1:
		FreeLibrary(hDll);
		continue;
	Insert:
		EInfo("[Preload] Find extension '{}' ({}, author: '{}'), at '{}'", ext->getName(), ext->getVersion().toString(), ext->getAuthor(), f.string());
		Detail::nameToDll.insert({ ext->getName(), f.string() });
		goto Free1;
	}

	Detail::initialized = true;
}

void ExtensionLoader::shutdown() {
	if (!Detail::initialized) {
		EWarn("ExtensionLoader::shutdown() called but the loader is already shut down.");
		return;
	}

	for (auto& [name, ext] : Detail::loadedExts) {
		if (ext) {
			ext->onShutdown();
			EInfo("Extension '{}' shut down", name);
		}
	}

	Detail::loadedExts.clear();
	Detail::nameToDll.clear();

	Detail::initialized = false;
}

Extension* ExtensionLoader::load(const String& name) {
	if (!Detail::initialized) {
		initialize();
		if (!Detail::initialized) {
			EError("Initialized failed, the loader cannot load any extensions");
			return nullptr;
		}
	}

	if (auto lext = Detail::loadedExts.find(name); lext != Detail::loadedExts.end()) {
		return lext->second;
	}

	auto dllName = Detail::nameToDll.find(name);
	if (dllName == Detail::nameToDll.end()) {
		EError("Cannot find extension '{}' in the map", name);
		return nullptr;
	}

	HMODULE hDll = LoadLibraryA(dllName->second.string().c_str());
	if (!hDll) {
		EError("Cannot load extension dll '{}': {:X}", dllName->second.string().c_str(), GetLastError());
		return nullptr;
	}
	auto pApi = (eeExtGetExtensionPtrFuncPtr)GetProcAddress(hDll, GetExtPtrFuncNameStr);
	if (!pApi) {
		pApi = (eeExtGetExtensionPtrFuncPtr)GetProcAddress(hDll, ("_" + String(GetExtPtrFuncNameStr)).c_str());
	}
	if (!pApi) {
		EError("Cannot find function '{}' in extension '{}'", GetExtPtrFuncNameStr, name);
		FreeLibrary(hDll);
		return nullptr;
	}
	if (!pApi()) {
		EError("Returned pointer is null");
		FreeLibrary(hDll);
		return nullptr;
	}
	Extension* ext = (Extension*)pApi();
	Detail::loadedExts.insert({ name, ext });
	ext->onLoad();
	return ext;
}

EE_NAMESPACE_END

#endif // EE_WINDOWS