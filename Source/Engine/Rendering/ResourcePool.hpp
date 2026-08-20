#pragma once

#include <Core/Types.hpp>

namespace EnderEngine::Rendering::Detail {

template <typename T>
class ResourcePool {
public:
	explicit ResourcePool(UInt32 initialCapacity = 256) {
		m_slots.reserve(initialCapacity);
		m_slots.resize(initialCapacity);
	}

	/**
	 * @brief Allocate a slot and return {index, generation}.
	 */
	struct Allocation {
		UInt32 index;
		UInt32 generation;
	};

	Allocation allocate() {
		UInt32 index;
		if (m_freeList.empty()) {
			index = static_cast<UInt32>(m_slots.size());
			// Grow capacity before emplacing to batch-reserve (avoids per-slot realloc)
			if (index >= m_slots.capacity())
				m_slots.reserve(m_slots.capacity() + 1024);
			m_slots.emplace_back();
		} else {
			index = m_freeList.back();
			m_freeList.pop_back();
		}
		m_slots[index].generation++;
		m_slots[index].used = true;
		return { index, m_slots[index].generation };
	}

	void release(UInt32 index) {
		if (index < m_slots.size() && m_slots[index].used) {
			m_slots[index].used = false;
			m_slots[index].resource = T{};
			m_freeList.push_back(index);
		}
	}

	bool isValid(UInt32 index, UInt32 generation) const {
		return index < m_slots.size()
			&& m_slots[index].used
			&& m_slots[index].generation == generation;
	}

	T* get(UInt32 index, UInt32 generation) {
		if (!isValid(index, generation)) return nullptr;
		return &m_slots[index].resource;
	}

	const T* get(UInt32 index, UInt32 generation) const {
		if (!isValid(index, generation)) return nullptr;
		return &m_slots[index].resource;
	}

	/**
	 * @brief Get direct pointer to slot's resource data (bypasses generation check).
	 * Use only during allocation for initialization.
	 */
	T* getUnchecked(UInt32 index) {
		if (index < m_slots.size()) return &m_slots[index].resource;
		return nullptr;
	}

	/**
	 * @brief Get the generation for a given index.
	 */
	UInt32 getGeneration(UInt32 index) const {
		if (index < m_slots.size()) return m_slots[index].generation;
		return 0;
	}

	UInt32 usedCount() const {
		UInt32 count = 0;
		for (auto& slot : m_slots) {
			if (slot.used) count++;
		}
		return count;
	}

	void reset() {
		m_slots.clear();
		m_freeList.clear();
	}

private:
	struct Slot {
		UInt32 generation = 0;
		bool used = false;
		T resource;
	};

	Vector<Slot> m_slots;
	Vector<UInt32> m_freeList;
};

} // namespace EnderEngine::Rendering::Detail
