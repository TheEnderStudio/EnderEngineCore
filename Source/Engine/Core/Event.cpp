#include <Engine/Core/Event.hpp>

EE_NAMESPACE_BEGIN

EventBus::EventBus() = default;

EventBus::~EventBus() = default;

void EventBus::unsubscribe(SubscriptionId id) {
	std::lock_guard<Mutex> lock(m_mutex);
	for (auto& pair : m_dispatchers) {
		pair.second->remove(id);
	}
}

void EventBus::clear() {
	std::lock_guard<Mutex> lock(m_mutex);
	m_dispatchers.clear();
}

SubscriptionId EventBus::allocateSubscriptionId() {
	return m_nextSubscriptionId.fetch_add(1, std::memory_order_relaxed);
}

EE_NAMESPACE_END
