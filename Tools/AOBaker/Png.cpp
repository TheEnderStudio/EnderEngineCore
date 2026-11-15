#include "Png.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <cstdlib>

namespace aob {

std::vector<std::uint8_t> encodePng(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgb) {
	if (width == 0 || height == 0 || rgb == nullptr) return {};
	int size = 0;
	unsigned char* memory = stbi_write_png_to_mem(rgb, static_cast<int>(width) * 3,
		static_cast<int>(width), static_cast<int>(height), 3, &size);
	if (memory == nullptr || size <= 0) return {};
	std::vector<std::uint8_t> out(memory, memory + size);
	STBIW_FREE(memory);
	return out;
}

} // namespace aob
