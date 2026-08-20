#include <Engine/Core/Guid.hpp>

#include <cstring>

EE_NAMESPACE_BEGIN

Guid::Guid() {
	std::memset(&m_uuid, 0, sizeof(m_uuid));
}

Guid::Guid(const UUIDv4::UUID& uuid) : m_uuid(uuid) {
}

Guid Guid::generate() {
	static UUIDv4::UUIDGenerator<std::mt19937_64> s_generator;
	return Guid(s_generator.getUUID());
}

Optional<Guid> Guid::parse(StringView str) {
	if (str.size() != 36) {
		return NullOpt;
	}
	try {
		UUIDv4::UUID uuid = UUIDv4::UUID::fromStrFactory(str.data());
		return Guid(uuid);
	}
	catch (...) {
		return NullOpt;
	}
}

String Guid::toString() const {
	return m_uuid.str();
}

bool Guid::isNil() const {
	static const UUIDv4::UUID s_nil(0ULL, 0ULL);
	return m_uuid == s_nil;
}

bool Guid::operator==(const Guid& other) const {
	return m_uuid == other.m_uuid;
}

bool Guid::operator!=(const Guid& other) const {
	return m_uuid != other.m_uuid;
}

bool Guid::operator<(const Guid& other) const {
	return m_uuid < other.m_uuid;
}

EE_NAMESPACE_END
