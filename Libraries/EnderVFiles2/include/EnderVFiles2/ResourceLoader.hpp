#pragma once

#include "Types.hpp"

#include <concepts>
#include <string_view>
#include <type_traits>

namespace EnderVFiles2
{
	struct ResourceLoadContext
	{
		std::string_view path;
		ByteSpan data;
	};

	namespace Detail
	{
		template <typename>
		inline constexpr bool AlwaysFalse = false;
	}

	template <typename Resource>
	struct ResourceLoader
	{
		static Resource load(const ResourceLoadContext&)
		{
			static_assert(Detail::AlwaysFalse<Resource>,
				"Specialize EnderVFiles2::ResourceLoader<Resource> and provide "
				"static Resource load(const EnderVFiles2::ResourceLoadContext&).");
		}
	};

	template <typename Resource>
	concept LoadableResource = !std::is_void_v<Resource>
		&& !std::is_reference_v<Resource>
		&& !std::is_const_v<Resource>
		&& std::is_move_constructible_v<Resource>;
}
