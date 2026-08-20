#pragma once

#include "Macros.h"
#include "Types.hpp"

#include <functional>
#include <atomic>
#include <string_view>

EE_NAMESPACE_BEGIN

/**
 * @brief Base class for all engine events.
 *
 * Concrete event types should derive from Event publicly so that they can be
 * published and subscribed to through the EventBus.
 */
class EE_API Event {
public:
	EE_DEFAULT_CON_DES(Event)
	EE_DEFAULT_COPY(Event)
	EE_DEFAULT_MOVE(Event)
};

/**
 * @brief Opaque handle representing an event subscription.
 */
using SubscriptionId = UInt64;

/**
 * @brief Invalid/null subscription handle.
 */
inline constexpr SubscriptionId InvalidSubscriptionId = 0;

namespace Detail {

using EventTypeId = Size;

inline EventTypeId getNextEventTypeId() {
	static std::atomic<EventTypeId> s_counter{0};
	return s_counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Detail

/**
 * @brief Get a unique runtime identifier for an event type.
 *
 * Uses a hash of the compiler-generated function signature string,
 * ensuring the same type produces the same ID regardless of which
 * translation unit (DLL or EXE) the template is instantiated in.
 * @tparam T The concrete event type.
 * @return A unique EventTypeId for T, consistent within the process.
 */
template <typename T>
Detail::EventTypeId getEventTypeId() {
	static const Detail::EventTypeId s_id = std::hash<std::string_view>{}("EE_" __FUNCDNAME__);
	return s_id;
}

// Forward declaration of the type-erased dispatcher interface.
class IEventDispatcher;

/**
 * @brief Type-safe event bus supporting publish/subscribe semantics.
 *
 * EventBus stores dispatchers per event type. Subscribers receive only the
 * event types they registered for. The implementation does not rely on RTTI.
 */
class EE_API EventBus {
public:
	EventBus();
	~EventBus();

	EE_NO_COPY(EventBus)
	EE_NO_MOVE(EventBus)

	/**
	 * @brief Subscribe to events of type T.
	 * @tparam T Concrete event type derived from Event.
	 * @tparam Handler Callable with signature void(const T&).
	 * @param handler The callback invoked when an event of type T is published.
	 * @return A subscription handle that can be used to unsubscribe later.
	 */
	template <typename T, typename Handler>
		requires std::is_base_of_v<Event, T>
	SubscriptionId subscribe(Handler&& handler) {
		auto& dispatcher = getDispatcher<T>();
		SubscriptionId id = allocateSubscriptionId();
		dispatcher.add(id, std::forward<Handler>(handler));
		return id;
	}

	/**
	 * @brief Publish an event to all subscribers of its type.
	 * @tparam T Concrete event type derived from Event.
	 * @param event The event instance to publish.
	 */
	template <typename T>
		requires std::is_base_of_v<Event, T>
	void publish(const T& event) {
		IEventDispatcher* dispatcher = findDispatcher<T>();
		if (dispatcher) {
			dispatcher->dispatch(event);
		}
	}

	/**
	 * @brief Remove a subscription.
	 * @param id The subscription handle returned by subscribe().
	 */
	void unsubscribe(SubscriptionId id);

	/**
	 * @brief Remove all subscriptions from the bus.
	 */
	void clear();

private:
	template <typename T>
	class EventDispatcher;

	template <typename T>
	EventDispatcher<T>& getDispatcher();

	template <typename T>
	IEventDispatcher* findDispatcher();

	SubscriptionId allocateSubscriptionId();

	HashMap<Detail::EventTypeId, Uptr<IEventDispatcher>> m_dispatchers;
	Mutex m_mutex;
	std::atomic<SubscriptionId> m_nextSubscriptionId{1};
};

/**
 * @brief Internal base interface for type-erased event dispatchers.
 */
class EE_API IEventDispatcher {
public:
	virtual ~IEventDispatcher() = default;
	virtual void dispatch(const Event& event) = 0;
	virtual void remove(SubscriptionId id) = 0;
};

/**
 * @brief Concrete dispatcher for a specific event type.
 * @tparam T The concrete event type this dispatcher handles.
 */
template <typename T>
class EventBus::EventDispatcher : public IEventDispatcher {
public:
	using Callback = std::function<void(const T&)>;

	void add(SubscriptionId id, Callback callback) {
		std::lock_guard<Mutex> lock(m_mutex);
		m_callbacks.push_back({id, std::move(callback)});
	}

	void dispatch(const Event& event) override {
		const T& typedEvent = static_cast<const T&>(event);
		Vector<Callback> callbacks;
		{
			std::lock_guard<Mutex> lock(m_mutex);
			callbacks.reserve(m_callbacks.size());
			for (const auto& pair : m_callbacks) {
				callbacks.push_back(pair.second);
			}
		}
		for (auto& callback : callbacks) {
			callback(typedEvent);
		}
	}

	void remove(SubscriptionId id) override {
		std::lock_guard<Mutex> lock(m_mutex);
		for (auto it = m_callbacks.begin(); it != m_callbacks.end(); ++it) {
			if (it->first == id) {
				m_callbacks.erase(it);
				return;
			}
		}
	}

private:
	Mutex m_mutex;
	Vector<std::pair<SubscriptionId, Callback>> m_callbacks;
};

template <typename T>
EventBus::EventDispatcher<T>& EventBus::getDispatcher() {
	std::lock_guard<Mutex> lock(m_mutex);
	Detail::EventTypeId id = getEventTypeId<T>();
	auto it = m_dispatchers.find(id);
	if (it == m_dispatchers.end()) {
		auto dispatcher = std::make_unique<EventDispatcher<T>>();
		EventDispatcher<T>* ptr = dispatcher.get();
		m_dispatchers.emplace(id, std::move(dispatcher));
		return *ptr;
	}
	return *static_cast<EventDispatcher<T>*>(it->second.get());
}

template <typename T>
IEventDispatcher* EventBus::findDispatcher() {
	std::lock_guard<Mutex> lock(m_mutex);
	Detail::EventTypeId id = getEventTypeId<T>();
	auto it = m_dispatchers.find(id);
	if (it == m_dispatchers.end()) {
		return nullptr;
	}
	return it->second.get();
}

EE_NAMESPACE_END
