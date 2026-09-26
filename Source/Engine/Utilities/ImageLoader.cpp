#include <Utilities/ImageLoader.hpp>
#include <Core/Log.hpp>
#include <Resource/ResourcesManager.hpp>

#include <spng.h>
#include <stb_image.h>
#include <fstream>
#include <algorithm>
#include <cmath>

#include <DiligentTools/TextureLoader/interface/TextureUtilities.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>
#include <DiligentFX/Components/interface/ShadowMapManager.hpp>

EE_NAMESPACE_UTILITIES_BEGIN

DecodedImage decodePNG(const void* data, Size size) {
	DecodedImage result;

	spng_ctx* ctx = spng_ctx_new(0);
	if (!ctx) {
		EWarn("spng_ctx_new failed");
		return result;
	}

	int err = spng_set_png_buffer(ctx, data, size);
	if (err != SPNG_OK) {
		EWarn("spng_set_png_buffer failed: {}", spng_strerror(err));
		spng_ctx_free(ctx);
		return result;
	}

	struct spng_ihdr ihdr {};
	err = spng_get_ihdr(ctx, &ihdr);
	if (err != SPNG_OK) {
		EWarn("spng_get_ihdr failed: {}", spng_strerror(err));
		spng_ctx_free(ctx);
		return result;
	}

	result.width = ihdr.width;
	result.height = ihdr.height;

	size_t outLen = 0;
	err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &outLen);
	if (err != SPNG_OK) {
		EWarn("spng_decoded_image_size failed: {}", spng_strerror(err));
		spng_ctx_free(ctx);
		return result;
	}

	result.pixels.resize(outLen);
	err = spng_decode_image(ctx, result.pixels.data(), outLen, SPNG_FMT_RGBA8, 0);
	if (err != SPNG_OK) {
		EWarn("spng_decode_image failed: {}", spng_strerror(err));
		result.pixels.clear();
		spng_ctx_free(ctx);
		return result;
	}

	spng_ctx_free(ctx);
	EInfo("PNG decoded (libspng): {}x{} -> {} bytes", result.width, result.height, outLen);
	return result;
}

MipChain buildMipChain(DecodedImage image, bool srgb) {
	MipChain chain;
	if (!image.isValid() || image.width == 0 || image.height == 0) return chain;

	chain.width = image.width;
	chain.height = image.height;

	// sRGB <-> linear lookup, so the box filter runs in linear light. The encode
	// side uses the sRGB transfer function directly (only 1/4 of the texels of a
	// level need it, since each output texel consumes four inputs).
	auto toLinear = [srgb](UInt8 v) -> F32 {
		if (!srgb) return (F32)v / 255.0f;
		static const Vector<F32> lut = [] {
			Vector<F32> t(256);
			for (int i = 0; i < 256; i++) {
				F32 c = (F32)i / 255.0f;
				t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
			}
			return t;
			}();
		return lut[v];
	};
	auto toEncoded = [srgb](F32 c) -> UInt8 {
		c = glm::clamp(c, 0.0f, 1.0f);
		if (!srgb) return (UInt8)(c * 255.0f + 0.5f);
		F32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		return (UInt8)(glm::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
	};

	chain.levels.push_back(std::move(image.pixels));
	UInt32 w = image.width, h = image.height;
	while (w > 1 || h > 1) {
		const UInt32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
		const Vector<UInt8>& src = chain.levels.back();
		Vector<UInt8> dst((Size)nw * nh * 4);
		for (UInt32 y = 0; y < nh; y++) {
			for (UInt32 x = 0; x < nw; x++) {
				// 2x2 footprint, clamped at the right/bottom edge for odd sizes.
				const UInt32 x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
				const UInt32 y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
				const Size i00 = ((Size)y0 * w + x0) * 4, i10 = ((Size)y0 * w + x1) * 4;
				const Size i01 = ((Size)y1 * w + x0) * 4, i11 = ((Size)y1 * w + x1) * 4;
				for (int c = 0; c < 4; c++) {
					F32 sum = toLinear(src[i00 + c]) + toLinear(src[i10 + c])
						+ toLinear(src[i01 + c]) + toLinear(src[i11 + c]);
					dst[((Size)y * nw + x) * 4 + c] = toEncoded(sum * 0.25f);
				}
			}
		}
		chain.levels.push_back(std::move(dst));
		w = nw; h = nh;
	}
	return chain;
}

DecodedImage decodeImage(const void* data, Size size) {
	// Try libspng first for PNG
	DecodedImage img = decodePNG(data, size);
	if (img.isValid()) return img;

	// Fallback: stb_image
	int w = 0, h = 0, ch = 0;
	stbi_uc* pixels = stbi_load_from_memory(
		static_cast<const stbi_uc*>(data), (int)size, &w, &h, &ch, 4);
	if (pixels) {
		img.width = (UInt32)w;
		img.height = (UInt32)h;
		img.pixels.assign(pixels, pixels + (Size)w * h * 4);
		stbi_image_free(pixels);
		EInfo("Image decoded (stb): {}x{} -> {} bytes", w, h, img.pixels.size());
	}
	return img;
}

void* loadTexture(const String& filePath, void* device, bool sRGB) {
	Diligent::TextureLoadInfo loadInfo;
	loadInfo.IsSRGB = sRGB;
	Diligent::RefCntAutoPtr<Diligent::ITexture> tex;
	Diligent::CreateTextureFromFile(filePath.c_str(), loadInfo,
		static_cast<Diligent::IRenderDevice*>(device), &tex);
	if (!tex) { EWarn("Failed to load texture: {}", filePath); return nullptr; }
	auto* srv = tex->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
	srv->AddRef();
	return srv;
}

void* loadTexture(const ResPath& filePath, void* device, bool sRGB) {
	auto& rm = ResourcesManager::getInstance();
	auto r = rm.readFile(filePath);
	if (r.isErr()) { EError("Resource read failed: {}", filePath.path.string()); return nullptr; }
	auto& data = r.value();
	auto decoded = decodeImage(data.data(), data.size());
	if (!decoded.isValid()) { EError("Decode failed: {}", filePath.path.string()); return nullptr; }

	Diligent::TextureLoadInfo loadInfo;
	loadInfo.IsSRGB = sRGB;
	loadInfo.Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
	Diligent::TextureDesc td; td.Name = filePath.path.filename().string().c_str();
	td.Type = Diligent::RESOURCE_DIM_TEX_2D; td.Width = decoded.width; td.Height = decoded.height;
	td.Format = Diligent::TEX_FORMAT_RGBA8_UNORM; td.MipLevels = 1;
	td.BindFlags = Diligent::BIND_SHADER_RESOURCE; td.Usage = Diligent::USAGE_IMMUTABLE;
	Diligent::TextureSubResData srd; srd.pData = decoded.pixels.data(); srd.Stride = decoded.width * 4;
	Diligent::TextureData td2; td2.pSubResources = &srd; td2.NumSubresources = 1;
	Diligent::RefCntAutoPtr<Diligent::ITexture> tex;
	static_cast<Diligent::IRenderDevice*>(device)->CreateTexture(td, &td2, &tex);
	if (!tex) { EError("Texture creation failed: {}", filePath.path.string()); return nullptr; }
	auto* srv = tex->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
	srv->AddRef();
	return srv;
}

DecodedImage loadImage(const Path& path) {
	namespace fs = std::filesystem;
	if (!fs::exists(path)) {
		EError("'{}' does not exist", path.string());
		return {};
	}

	Size size = fs::file_size(path);
	if (size == 0) {
		EError("'{}' is empty", path.string());
		return {};
	}

	std::ifstream file(path, std::ios::binary);
	if (!file) {
		EError("Failed to open file '{}'", path.string());
		return {};
	}

	Vector<Byte> buffer(size);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	if (!file) {
		EError("Failed to read file '{}'", path.string());
		return {};
	}

	DecodedImage image = decodeImage(buffer.data(), size);
	if (!image.isValid()) {
		EError("Cannot decode image file '{}'", path.string());
		return {};
	}
	return image;
}

DecodedImage loadImage(const ResPath& path) {
	auto& rm = ResourcesManager::getInstance();
	auto r = rm.readFile(path);
	if (r.isErr()) { EError("Read file '{}' failed: {}", path.string(), ToString(r.error())); return {}; }
	auto& data = r.value();
	DecodedImage image = decodeImage(data.data(), data.size());
	if (!image.isValid()) {
		EError("Cannot decode image file '{}'", path.string());
		return {};
	}
	return image;
}

EE_NAMESPACE_UTILITIES_END
