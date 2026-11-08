#pragma once

#include <Engine/Core/I18n.hpp>
#include <Engine/Utilities/ImageLoader.hpp>
#include <Engine/Audio/AudioTypes.hpp>

EE_NAMESPACE_L10N_BEGIN

template <typename AssetType>
	requires std::copyable<AssetType> && std::default_initializable<AssetType>
class IAssetPool {
public:
	struct View {
		AssetType* ptr;
	};

	virtual View intern(View assetPtr) = 0;
	virtual void clear() = 0;
	virtual void reserve(Size size) = 0;
};

template <typename T, typename U>
concept ViewValid = requires(T t) {
	{ t->ptr } -> std::convertible_to<U>;
};

template <typename T>
	requires ViewValid<typename IAssetPool<T>::View, T>
class LocalizedAsset : public LocalizedAssetBase {
public:
	using AssetPool = IAssetPool<T>;
	using AssetView = AssetPool::View;

	LocalizedAsset(Uptr<AssetPool> pool, LanguageRegistry* reg = &LanguageRegistry::getInstance()) :
		m_reg(reg), m_pool(pool) {}

	AssetPool* pool() { return m_pool.get(); }
	const AssetPool* pool() const { return m_pool.get(); }

	LanguageRegistry* registry() { return m_reg; }
	const LanguageRegistry* registry() const { return m_reg; }

	void set(LanguageId id, AssetView ptr) {
		if (!ptr->ptr) return;
		std::unique_lock<SharedMutex> lock(m_mutex);
		AssetView view = m_pool->intern(ptr);
		m_l10n[id] = view;
	}

	void set(const HashMap<LanguageId, AssetView>& ptrs) {
		for (const auto& [id, ptr] : ptrs) {
			set(id, ptr);
		}
	}

	AssetView get(LanguageId id) const {
		std::shared_lock<SharedMutex> lock(m_mutex);
		auto it = m_l10n.find(id);
		return it == m_l10n.end() ? (m_l10n.contains(static_cast<LanguageId>(Language::English)) ? m_l10n.find(static_cast<LanguageId>(Language::English))->second : AssetView{}) : it->second;
	}

	AssetView get() const {
		return get(m_reg ? m_reg->currentLanguage() : static_cast<LanguageId>(Language::English));
	}

	void clear() {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n.clear();
	}

	void clearPool() {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n.clear();
		m_pool->clear();
	}

	void reserve(Size size) {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n.reserve(size);
	}

	void reservePool(Size size) {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_pool->reserve(size);
	}

	operator AssetView() {
		return get();
	}

private:
	mutable SharedMutex m_mutex;
	LanguageRegistry* m_reg;
	HashMap<LanguageId, AssetView> m_l10n;
	Uptr<AssetPool> m_pool;
};

EE_NAMESPACE_L10N_END
