#pragma once

#include <Core/Types.hpp>
#include "Errors.hpp"

EE_NAMESPACE_BEGIN

/// @brief Raw resource data buffer.
using ResData = Vector<Byte>;

/// @brief Async read callback: (virtualPath, data, error). Data is empty on failure.
using ResourceAsyncCallback = std::function<void(const ResPath&, ResData, ResourceError)>;

EE_NAMESPACE_END