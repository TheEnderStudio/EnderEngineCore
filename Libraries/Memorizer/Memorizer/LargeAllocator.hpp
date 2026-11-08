#pragma once

#include "Common.hpp"

#ifdef EE_WINDOWS
# include <Windows.h>
# include <memoryapi.h>
#else
#endif

#include <bitset>

EE_NAMESPACE_MEMORIZER_BEGIN

enum class PageClass : UInt8 { Normal4K = 0, Large2MB = 1, Huge1GB = 2 };

struct LargeAllocation {
	void*     base = nullptr;
	Size      reservedSize = 0;
	Size      committedSize = 0;
	PageClass pageClass = PageClass::Normal4K;
	Size      pageSize = 0;
	UInt64    id = 0;
	bool      faultCommit = false;
	explicit operator bool() const { return base != nullptr; }
};

struct LargeAllocStats {
	Size reservedBytes = 0;
	Size committedBytes = 0;
	Size blockCount = 0;
	Size hugeBlocks = 0;
	Size largeBlocks = 0;
	Size normalBlocks = 0;
};

namespace LDetail {

	EE_NODISCARD inline Size regularPageSize() {
		SYSTEM_INFO si{}; GetSystemInfo(&si);
		return si.dwPageSize ? si.dwPageSize : 4096;
	}

	inline bool enableLockPrivilege() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
			return false;
		LUID luid{};
		if (!LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid)) { CloseHandle(token); return false; }
		TOKEN_PRIVILEGES tp{};
		tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
		DWORD err = GetLastError();
		CloseHandle(token);
		return ok && err == ERROR_SUCCESS;
	}

	struct FaultRegion {
		uintptr_t begin;
		uintptr_t end;
		Size      granularity;
		Size      committed;
	};

    class FaultCommitRegistry {
    public:
        static FaultCommitRegistry& getInstance() {
            static FaultCommitRegistry instance;
            return instance;
        }

        void Register(uintptr_t begin, uintptr_t end, Size granularity) {
            AcquireSRWLockExclusive(&m_lock);
            m_regions[begin] = FaultRegion{ begin, end, granularity, 0 };
            ReleaseSRWLockExclusive(&m_lock);
            EnsureHandlerInstalled();
        }
        void Unregister(uintptr_t begin) {
            AcquireSRWLockExclusive(&m_lock);
            m_regions.erase(begin);
            ReleaseSRWLockExclusive(&m_lock);
        }
        Size QueryCommitted(uintptr_t begin) {
            AcquireSRWLockExclusive(&m_lock);
            auto it = m_regions.find(begin);
            Size v = (it != m_regions.end()) ? it->second.committed : 0;
            ReleaseSRWLockExclusive(&m_lock);
            return v;
        }

    private:
        FaultCommitRegistry() = default;

        SRWLOCK                     m_lock = SRWLOCK_INIT;
        Map<uintptr_t, FaultRegion> m_regions;
        PVOID                       m_handler = nullptr;
        std::atomic<bool>           m_installed{ false };
        Mutex                       m_installMutex;

        static LONG CALLBACK VehThunk(PEXCEPTION_POINTERS ep) {
            return getInstance().OnException(ep);
        }

        void EnsureHandlerInstalled() {
            if (m_installed.load(std::memory_order_acquire)) return;
            std::lock_guard<Mutex> g(m_installMutex);
            if (m_installed.load(std::memory_order_relaxed)) return;
            m_handler = AddVectoredExceptionHandler(1, &FaultCommitRegistry::VehThunk);
            m_installed.store(true, std::memory_order_release);
        }

        LONG OnException(PEXCEPTION_POINTERS ep) {
            auto* rec = ep->ExceptionRecord;
            if (!rec) return EXCEPTION_CONTINUE_SEARCH;
            if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
                rec->ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
                return EXCEPTION_CONTINUE_SEARCH;
            if (rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
            if (rec->ExceptionInformation[0] == 8) return EXCEPTION_CONTINUE_SEARCH;
            uintptr_t addr = (uintptr_t)rec->ExceptionInformation[1];
            return HandleFault(addr) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
        }

        bool HandleFault(uintptr_t addr) {
            AcquireSRWLockExclusive(&m_lock);
            auto it = m_regions.upper_bound(addr);
            if (it == m_regions.begin()) { ReleaseSRWLockExclusive(&m_lock); return false; }
            --it;
            FaultRegion& r = it->second;
            if (addr < r.begin || addr >= r.end) { ReleaseSRWLockExclusive(&m_lock); return false; }

            const Size ps = regularPageSize();
            Size gran = r.granularity ? r.granularity : ps;
            if (gran % ps) gran = AlignUp(gran, ps);

            uintptr_t offset = addr - r.begin;
            uintptr_t chunkStart = r.begin + (offset / gran) * gran;
            uintptr_t chunkEnd = chunkStart + gran;
            if (chunkEnd > r.end) chunkEnd = r.end;
            if (chunkStart < r.begin) chunkStart = r.begin;

            SIZE_T chunkSize = (SIZE_T)((chunkEnd - chunkStart + ps - 1) & ~(ps - 1));
            if (chunkStart + chunkSize > r.end) chunkSize = (SIZE_T)(r.end - chunkStart);
            void* p = VirtualAlloc((void*)chunkStart, chunkSize, MEM_COMMIT, PAGE_READWRITE);
            if (!p) { ReleaseSRWLockExclusive(&m_lock); return false; }
            r.committed += chunkSize;
            ReleaseSRWLockExclusive(&m_lock);
            return true;
        }
    };
} // namespace LDetail

class LargeAllocator {
public:
    struct Config {
        bool enableHuge1GB = true;
        bool enableLarge2MB = true;
        /// @brief Commit normal pages on the first touch instead of up front.
        ///
        /// Off by default. A deferred commit means the first *store* to a fresh block
        /// raises an access violation and is only made to work by this allocator's
        /// vectored handler; anything else in the process that installs a vectored
        /// handler, translates SEH into C++ exceptions, or runs with a debugger that
        /// treats the fault as fatal turns that into an "access violation during
        /// allocation" the caller cannot do anything about. One VirtualAlloc call is
        /// cheap next to that risk, so normal blocks are committed immediately; opt in
        /// only for blocks where the commit cost actually matters.
        bool enableFaultCommit = false;
        Size faultCommitGranularity = 2ull << 20; // 2 MB
        /// @brief Commit normal blocks up front even when fault commit is enabled.
        bool commitNormalImmediately = true;
    };

    explicit LargeAllocator(Config cfg = {}) : m_cfg(cfg) {
        LDetail::enableLockPrivilege();
        probeLargePageSupport();
    }
    ~LargeAllocator() { freeAll(); }

    EE_NO_COPY(LargeAllocator);

    bool allocate(Size size, LargeAllocation& out) {
        if (size == 0 || size > SIZE_MAX / 2) return false;
        std::lock_guard<Mutex> lock(m_mutex);

        if (m_cfg.enableHuge1GB && hugeWorthIt(size) && m_hugeSupport.load(std::memory_order_relaxed) != ProbeFailed) {
            if (tryHuge1GB(size, out)) return Register(out);
            m_hugeSupport.store(ProbeFailed, std::memory_order_relaxed);
        }
        if (m_cfg.enableLarge2MB && largeWorthIt(size)) {
            if (tryLarge2MB(size, out)) return Register(out);
        }
        if (tryNormal(size, out)) return Register(out);
        return false;
    }

    bool commit(LargeAllocation& alloc, Size offset, Size size) {
        if (!alloc || size == 0) return false;
        if (offset > alloc.reservedSize || size > alloc.reservedSize - offset) return false;
        if (alloc.pageClass != PageClass::Normal4K) return true;

        std::lock_guard<Mutex> lock(m_mutex);
        const Size ps = LDetail::regularPageSize();
        const Size start = offset & ~(ps - 1);
        const Size end = AlignUp(offset + size, ps);
        void* p = VirtualAlloc(static_cast<UInt8*>(alloc.base) + start,
            end - start, MEM_COMMIT, PAGE_READWRITE);
        if (!p) return false;
        alloc.committedSize += (end - start);
        for (auto& a : m_allocs) if (a.id == alloc.id) { a.committedSize = alloc.committedSize; break; }
        return true;
    }

    void free(LargeAllocation& alloc) {
        if (!alloc) return;
        std::lock_guard<Mutex> lock(m_mutex);
        releaseLocked(alloc);
        alloc = LargeAllocation{};
    }

    bool free(void* ptr) {
        if (!ptr) return true;
        std::lock_guard<Mutex> lock(m_mutex);
        for (auto it = m_allocs.begin(); it != m_allocs.end(); ++it) {
            if (it->base != ptr) continue;
            // Release the block we found, then remove it. Going through
            // free(LargeAllocation&) here re-locked m_mutex on a non-recursive mutex -
            // every engineFree() of a large block deadlocked - and erased from the
            // container while the caller's iterator was still live.
            releaseLocked(*it);
            m_allocs.erase(it);
            return true;
        }
        return false;
    }

    void freeAll() {
        std::lock_guard<Mutex> lock(m_mutex);
        for (auto& a : m_allocs) releaseLocked(a);
        m_allocs.clear();
    }

    bool isAllocByThis(void* ptr) const {
        if (!ptr) return false;
        std::lock_guard<Mutex> lock(m_mutex);
        for (const auto& a : m_allocs) {
            if (a.base == ptr) return true;
        }
        return false;
    }

    LargeAllocStats getStats() const {
        std::lock_guard<Mutex> lock(m_mutex);
        LargeAllocStats s{};
        for (auto& a : m_allocs) {
            s.reservedBytes += a.reservedSize;
            s.committedBytes += a.committedSize;
            if (a.faultCommit)
                s.committedBytes += LDetail::FaultCommitRegistry::getInstance().QueryCommitted((uintptr_t)a.base);
            ++s.blockCount;
            switch (a.pageClass) {
            case PageClass::Huge1GB:  ++s.hugeBlocks;   break;
            case PageClass::Large2MB: ++s.largeBlocks;  break;
            default:                  ++s.normalBlocks; break;
            }
        }
        return s;
    }

    bool isHugePageAvailable()  const { return m_hugeSupport.load(std::memory_order_relaxed) == ProbeOk; }
    bool isLargePageAvailable() const { return m_largePageSize != 0; }
    void resetHugePageProbe() { m_hugeSupport.store(ProbeUnknown, std::memory_order_relaxed); }

private:
    enum ProbeState : int { ProbeUnknown = -1, ProbeFailed = 0, ProbeOk = 1 };

    Config                  m_cfg;
    mutable Mutex           m_mutex;
    Vector<LargeAllocation> m_allocs;
    UInt64                  m_nextId = 1;
    Size                    m_largePageSize = 0;
    std::atomic<int>        m_hugeSupport{ ProbeUnknown };

    void probeLargePageSupport() {
        m_largePageSize = GetLargePageMinimum();
        if (m_largePageSize == 0) return;
        void* p = VirtualAlloc(nullptr, m_largePageSize,
            MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (p) VirtualFreeEx(GetCurrentProcess(), p, 0, MEM_RELEASE);
        else   m_largePageSize = 0;
    }


    bool Register(LargeAllocation& out) { out.id = m_nextId++; m_allocs.push_back(out); return true; }

    /// @brief Release a block without taking m_mutex (the caller holds it).
    void releaseLocked(LargeAllocation& a) {
        if (!a.base) return;
        if (a.faultCommit)
            LDetail::FaultCommitRegistry::getInstance().Unregister((uintptr_t)a.base);
        VirtualFreeEx(GetCurrentProcess(), a.base, 0, MEM_RELEASE);
        // Invalidate the record so a stale copy cannot be released twice; the caller
        // removes it from m_allocs.
        a.base = nullptr;
        a.faultCommit = false;
        a.reservedSize = a.committedSize = 0;
    }

    /// @brief Whether a request is big enough to justify burning a scarce huge page.
    ///
    /// Only request size decides: the old code tried a 1 GB huge-page allocation for
    /// *every* block (rounding a 300 KB request up to 1 GB), which on a desktop OS
    /// fails slowly and, if it ever succeeded, reserved a gigabyte for it. Same for the
    /// 2 MB pages, which a 256 KB request would have consumed.
    static bool hugeWorthIt(Size size) { return size >= ((Size)1 << 30); }
    bool largeWorthIt(Size size) const { return m_largePageSize != 0 && size >= m_largePageSize; }

    bool tryHuge1GB(Size size, LargeAllocation& out) {
        constexpr Size kHuge = (Size)1 << 30;
        const Size allocSize = AlignUp(size, kHuge);
        if (allocSize == 0) return false;

        MEM_EXTENDED_PARAMETER ext{};
        ext.Type = MemExtendedParameterAttributeFlags;
        ext.ULong64 = MEM_EXTENDED_PARAMETER_NONPAGED_HUGE;

        void* p = VirtualAlloc2(GetCurrentProcess(), nullptr, allocSize,
            MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE, &ext, 1);
        if (!p) return false;

        m_hugeSupport.store(ProbeOk, std::memory_order_relaxed);
        out.base = p; out.reservedSize = allocSize; out.committedSize = allocSize;
        out.pageClass = PageClass::Huge1GB; out.pageSize = kHuge;
        return true;
    }

    bool tryLarge2MB(Size size, LargeAllocation& out) {
        const Size allocSize = AlignUp(size, m_largePageSize);
        if (allocSize == 0) return false;
        void* p = VirtualAlloc(nullptr, allocSize,
            MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!p) return false;
        out.base = p; out.reservedSize = allocSize; out.committedSize = allocSize;
        out.pageClass = PageClass::Large2MB; out.pageSize = m_largePageSize;
        return true;
    }

    bool tryNormal(Size size, LargeAllocation& out) {
        const Size ps = LDetail::regularPageSize();
        const Size allocSize = AlignUp(size, ps);
        if (allocSize == 0) return false;

        void* p = VirtualAlloc(nullptr, allocSize, MEM_RESERVE, PAGE_READWRITE);
        if (!p) return false;

        out.base = p; out.reservedSize = allocSize; out.committedSize = 0;
        out.pageClass = PageClass::Normal4K; out.pageSize = ps; out.faultCommit = false;

        // Fault commit commits a chunk on the first touch. The chunk has to be clamped
        // to the block: the old code only registered when the (2 MB default)
        // granularity fit inside the allocation, so every request between 256 KB and
        // 2 MB came back *reserved but never committed* - the first store was an access
        // violation with no handler. Anything not covered by the fault path is now
        // committed here.
        bool useFaultCommit = false;
        if (m_cfg.enableFaultCommit) {
            Size gran = m_cfg.faultCommitGranularity;
            if (gran < ps) gran = ps;
            gran = AlignUp(gran, ps);
            if (gran > allocSize) gran = allocSize;
            if (gran >= ps) {
                LDetail::FaultCommitRegistry::getInstance().Register(
                    (uintptr_t)p, (uintptr_t)p + allocSize, gran);
                out.faultCommit = useFaultCommit = true;
            }
        }
        if (!useFaultCommit || m_cfg.commitNormalImmediately) {
            void* c = VirtualAlloc(p, allocSize, MEM_COMMIT, PAGE_READWRITE);
            if (!c) { VirtualFreeEx(GetCurrentProcess(), p, 0, MEM_RELEASE); out = LargeAllocation{}; return false; }
            if (useFaultCommit) {
                // Committed up front: the fault handler has nothing left to do.
                LDetail::FaultCommitRegistry::getInstance().Unregister((uintptr_t)p);
                out.faultCommit = false;
            }
            out.committedSize = allocSize;
        }
        return true;
    }
};

EE_NAMESPACE_MEMORIZER_END
