#include <Engine/Core/Object.hpp>

EE_NAMESPACE_BEGIN

Object::Object() : m_guid(Guid::generate()) {
}

Object::Object(const Guid& guid) : m_guid(guid) {
}

EE_NAMESPACE_END
