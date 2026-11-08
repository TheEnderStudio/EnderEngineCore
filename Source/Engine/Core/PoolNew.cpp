// ======================================================================================
// Optional global heap replacement (EE_POOL_GLOBAL_NEW, `xmake f --pool_allocator=y`).
//
// This file is compiled into *every module that exchanges owning memory with the engine*
// (the engine DLL, and the executables / extensions that are built with the option on).
// That is what makes the switch coherent: the definitions below are per-module, but they
// all forward to the engine's exported engineAllocate/engineFree, so the whole process
// shares one set of pools.
//
// Why it cannot live in Allocation.cpp alone: `operator new` in the engine DLL is not
// visible to the Demo or to the tests, so those modules would keep using the CRT. A
// std::string created inside the engine would then be freed by the caller with the CRT
// allocator (debug-heap abort, or silent corruption) - which is exactly what happened the
// first time this switch was enabled.
// ======================================================================================
#ifdef EE_POOL_GLOBAL_NEW

// The full path, because this file is compiled into modules (Demo, tests, extensions)
// that do not have Include/Engine on their include path.
#include <Engine/Core/Allocation.hpp>

#include <cstddef>
#include <new>

namespace {
	inline void* poolGlobalNew(std::size_t n) {
		if (void* p = EnderEngine::engineAllocate(n)) return p;
		EnderEngine::Detail::poolOutOfMemory();
	}
	inline void* poolGlobalNewAligned(std::size_t n, std::size_t alignment) {
		if (void* p = EnderEngine::engineAllocateAligned(n, alignment)) return p;
		EnderEngine::Detail::poolOutOfMemory();
	}
} // namespace

void* operator new(std::size_t n) { return poolGlobalNew(n); }
void* operator new[](std::size_t n) { return poolGlobalNew(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return EnderEngine::engineAllocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return EnderEngine::engineAllocate(n); }
void* operator new(std::size_t n, std::align_val_t al) { return poolGlobalNewAligned(n, static_cast<std::size_t>(al)); }
void* operator new[](std::size_t n, std::align_val_t al) { return poolGlobalNewAligned(n, static_cast<std::size_t>(al)); }

void operator delete(void* p) noexcept { EnderEngine::engineFree(p); }
void operator delete[](void* p) noexcept { EnderEngine::engineFree(p); }
void operator delete(void* p, std::size_t) noexcept { EnderEngine::engineFree(p); }
void operator delete[](void* p, std::size_t) noexcept { EnderEngine::engineFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { EnderEngine::engineFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { EnderEngine::engineFree(p); }
void operator delete(void* p, std::align_val_t al) noexcept { EnderEngine::engineFreeAligned(p, static_cast<std::size_t>(al)); }
void operator delete[](void* p, std::align_val_t al) noexcept { EnderEngine::engineFreeAligned(p, static_cast<std::size_t>(al)); }
void operator delete(void* p, std::size_t, std::align_val_t al) noexcept { EnderEngine::engineFreeAligned(p, static_cast<std::size_t>(al)); }
void operator delete[](void* p, std::size_t, std::align_val_t al) noexcept { EnderEngine::engineFreeAligned(p, static_cast<std::size_t>(al)); }

#endif // EE_POOL_GLOBAL_NEW
