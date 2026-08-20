#include <Utilities/ImageLoader.hpp>
#include <Core/Log.hpp>
#include <Resource/ResourcesManager.hpp>

#include <spng.h>
#include <stb_image.h>
#include <fstream>

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
