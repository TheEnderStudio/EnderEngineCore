#pragma once

#include <Engine/Core/Types.hpp>
#include <Engine/Core/Log.hpp>

EE_NAMESPACE_UTILITIES_BEGIN

template <typename Key, typename Value>
class LRUCache {
public:
	explicit LRUCache(Size capacity) : m_capacity(capacity) {
		if (capacity == 0) {
			EWarn("LRUCache object created with ZERO capacity (modified to 1)");
			m_capacity = 1;
		}
	}

	Optional<Value> get(const Key& k) {
		std::unique_lock lock(m_mutex);
		auto it = m_map.find(k);
		if (it == m_map.end()) {
			return std::nullopt;
		}

		m_list.splice(m_list.begin(), m_list, it->second);
		return it->second->second;
	}

	void put(const Key& k, const Value& v) {
		std::unique_lock lock(m_mutex);
		auto it = m_map.find(k);

		if (it != m_map.end()) {
			it->second->second = v;
			m_list.splice(m_list.begin(), m_list, it->second);
			return;
		}

		if (m_list.size() == m_capacity) {
			auto last = std::prev(m_list.end());
			m_map.erase(last->first);
			m_list.pop_back();
		}

		m_list.emplace_front(k, v);
		m_map.emplace(k, m_list.begin());
	}

	Size size() const {
		std::shared_lock lock(m_mutex);
		return m_list.size();
	}

	void clear() {
		std::unique_lock lock(m_mutex);
		m_list.clear();
		m_map.clear();
	}

	Size capacity() const {
		return m_capacity;
	}

private:
	Size m_capacity;
	List<std::pair<Key, Value>> m_list;
	HashMap<Key, typename List<std::pair<Key, Value>>::iterator> m_map;
	mutable SharedMutex m_mutex;
};

EE_NAMESPACE_UTILITIES_END