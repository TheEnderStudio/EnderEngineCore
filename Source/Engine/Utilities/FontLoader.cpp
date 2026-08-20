#include <Utilities/FontLoader.hpp>
#include <Core/Log.hpp>
#include <Resource/ResourcesManager.hpp>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

namespace D = Diligent;

EE_NAMESPACE_UTILITIES_BEGIN

FontData loadFont(const String& fontPath, F32 fontSize, void* devicePtr, CharSet sets, const String& extra) {
	FontData result; result.fontSize = fontSize;

	FT_Library ft; if(FT_Init_FreeType(&ft)){EError("FT init");return result;}
	FT_Face face; if(FT_New_Face(ft,fontPath.c_str(),0,&face)){EError("FT load: {}",fontPath);FT_Done_FreeType(ft);return result;}
	FT_Set_Pixel_Sizes(face,0,(FT_UInt)fontSize);
	result.lineHeight = (F32)(face->size->metrics.height>>6);

	HashSet<UInt32> needed;
	if(int(sets & CharSet::ASCII)) for(UInt32 c=0x20;c<0x7F;++c) needed.insert(c);
	if(int(sets & CharSet::Latin1)) for(UInt32 c=0xA0;c<0x100;++c) needed.insert(c);
	if(int(sets & CharSet::CJK))    for(UInt32 c=0x4E00;c<=0x9FFF;++c) needed.insert(c);
	if(int(sets & CharSet::Custom)) for(char32_t cp : extra) needed.insert((UInt32)cp);
	if(needed.empty()) for(UInt32 c=0x20;c<0x7F;++c) needed.insert(c); // fallback ASCII

	const UInt32 atlasW = 2048, charH = (UInt32)(fontSize*1.5f+4);
	UInt32 atlasH = charH, penX = 0, penY = 0;
	Vector<UInt8> atlasRGBA(atlasW * atlasH * 4, 0);
	for(UInt32 i=0;i<atlasW*atlasH;++i){atlasRGBA[i*4+0]=255;atlasRGBA[i*4+1]=255;atlasRGBA[i*4+2]=255;}

	UInt32 loaded=0;
	for(UInt32 cp : needed) {
		if(FT_Load_Char(face,cp,FT_LOAD_RENDER)) continue;
		FT_Bitmap& bmp=face->glyph->bitmap; UInt32 gw=bmp.width,gh=bmp.rows;
		if(penX+gw+1>atlasW){penX=0;penY+=charH;atlasH=penY+charH;atlasRGBA.resize(atlasW*atlasH*4,0);
			for(UInt32 i=(atlasH-charH)*atlasW;i<atlasW*atlasH;++i){atlasRGBA[i*4+0]=255;atlasRGBA[i*4+1]=255;atlasRGBA[i*4+2]=255;}}
		Glyph g; g.size=Vec2((F32)gw,(F32)gh); g.bearing=Vec2((F32)face->glyph->bitmap_left,(F32)face->glyph->bitmap_top);
		g.advance=(F32)(face->glyph->advance.x>>6); g.uvMin=Vec2((F32)penX,(F32)penY);
		if(gw>0&&gh>0) for(UInt32 y=0;y<gh;++y)for(UInt32 x=0;x<gw;++x){UInt32 pi=((penY+y)*atlasW+(penX+x))*4;atlasRGBA[pi+0]=255;atlasRGBA[pi+1]=255;atlasRGBA[pi+2]=255;atlasRGBA[pi+3]=bmp.buffer[y*bmp.pitch+x];}
		result.glyphs[cp]=g; penX+=gw+1; ++loaded;
	}

	result.atlasWidth=atlasW; result.atlasHeight=atlasH;
	for(auto& p:result.glyphs){auto& g=p.second;float px=g.uvMin.x,py=g.uvMin.y;g.uvMin=Vec2(px/atlasW,py/atlasH);g.uvMax=Vec2((px+g.size.x)/atlasW,(py+g.size.y)/atlasH);}

	{ auto* dev=static_cast<D::IRenderDevice*>(devicePtr);
	  D::TextureDesc td; td.Name="FontAtlas"; td.Type=D::RESOURCE_DIM_TEX_2D; td.Width=atlasW; td.Height=atlasH; td.MipLevels=1;
	  td.Format=D::TEX_FORMAT_RGBA8_UNORM; td.BindFlags=D::BIND_SHADER_RESOURCE; td.Usage=D::USAGE_IMMUTABLE;
	  D::TextureSubResData srd; srd.pData=atlasRGBA.data(); srd.Stride=atlasW*4; D::TextureData tdata; tdata.pSubResources=&srd; tdata.NumSubresources=1;
	  D::RefCntAutoPtr<D::ITexture> tex; dev->CreateTexture(td,&tdata,&tex);
	  if(tex){auto* srv=tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);srv->AddRef();result.atlasSRV=srv;} }

	FT_Done_Face(face); FT_Done_FreeType(ft);
	EInfo("Font: {} ({}px, {}x{}, {} glyphs)",fontPath,fontSize,atlasW,atlasH,result.glyphs.size());
	return result;
}

FontData loadFont(const ResPath& fontPath, F32 fontSize, void* devicePtr, CharSet sets, const String& extra) {
	auto& rm = ResourcesManager::getInstance();
	auto r = rm.readFile(fontPath);
	if (r.isErr()) { EError("Font resource read failed: {}", fontPath.path.string()); return {}; }
	auto& data = r.value();

	FontData result; result.fontSize = fontSize;
	FT_Library ft; if (FT_Init_FreeType(&ft)) { EError("FT init"); return result; }
	FT_Face face; if (FT_New_Memory_Face(ft, data.data(), (FT_Long)data.size(), 0, &face)) {
		EError("FT memory load: {}", fontPath.path.string()); FT_Done_FreeType(ft); return result;
	}
	FT_Set_Pixel_Sizes(face, 0, (FT_UInt)fontSize);
	result.lineHeight = (F32)(face->size->metrics.height >> 6);

	HashSet<UInt32> needed;
	if (int(sets & CharSet::ASCII))  for (UInt32 c = 0x20; c < 0x7F; ++c) needed.insert(c);
	if (int(sets & CharSet::Latin1)) for (UInt32 c = 0xA0; c < 0x100; ++c) needed.insert(c);
	if (int(sets & CharSet::CJK))    for (UInt32 c = 0x4E00; c <= 0x9FFF; ++c) needed.insert(c);
	if (int(sets & CharSet::Custom)) for (char32_t cp : extra) needed.insert((UInt32)cp);
	if (needed.empty()) for (UInt32 c = 0x20; c < 0x7F; ++c) needed.insert(c);

	const UInt32 atlasW = 2048, charH = (UInt32)(fontSize * 1.5f + 4);
	UInt32 atlasH = charH, penX = 0, penY = 0;
	Vector<UInt8> atlasRGBA(atlasW * atlasH * 4, 0);
	for (UInt32 i = 0; i < atlasW * atlasH; ++i) { atlasRGBA[i * 4 + 0] = 255; atlasRGBA[i * 4 + 1] = 255; atlasRGBA[i * 4 + 2] = 255; }

	UInt32 loaded = 0;
	for (UInt32 cp : needed) {
		if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) continue;
		FT_Bitmap& bmp = face->glyph->bitmap; UInt32 gw = bmp.width, gh = bmp.rows;
		if (penX + gw + 1 > atlasW) { penX = 0; penY += charH; atlasH = penY + charH; atlasRGBA.resize(atlasW * atlasH * 4, 0);
			for (UInt32 i = (atlasH - charH) * atlasW; i < atlasW * atlasH; ++i) { atlasRGBA[i * 4 + 0] = 255; atlasRGBA[i * 4 + 1] = 255; atlasRGBA[i * 4 + 2] = 255; } }
		Glyph g; g.size = Vec2((F32)gw, (F32)gh); g.bearing = Vec2((F32)face->glyph->bitmap_left, (F32)face->glyph->bitmap_top);
		g.advance = (F32)(face->glyph->advance.x >> 6); g.uvMin = Vec2((F32)penX, (F32)penY);
		if (gw > 0 && gh > 0) for (UInt32 y = 0; y < gh; ++y) for (UInt32 x = 0; x < gw; ++x) { UInt32 pi = ((penY + y) * atlasW + (penX + x)) * 4; atlasRGBA[pi + 0] = 255; atlasRGBA[pi + 1] = 255; atlasRGBA[pi + 2] = 255; atlasRGBA[pi + 3] = bmp.buffer[y * bmp.pitch + x]; }
		result.glyphs[cp] = g; penX += gw + 1; ++loaded;
	}

	result.atlasWidth = atlasW; result.atlasHeight = atlasH;
	for (auto& p : result.glyphs) { auto& g = p.second; float px = g.uvMin.x, py = g.uvMin.y; g.uvMin = Vec2(px / atlasW, py / atlasH); g.uvMax = Vec2((px + g.size.x) / atlasW, (py + g.size.y) / atlasH); }

	{
		auto* dev = static_cast<D::IRenderDevice*>(devicePtr);
		D::TextureDesc td; td.Name = "FontAtlas"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = atlasW; td.Height = atlasH; td.MipLevels = 1;
		td.Format = D::TEX_FORMAT_RGBA8_UNORM; td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
		D::TextureSubResData srd; srd.pData = atlasRGBA.data(); srd.Stride = atlasW * 4; D::TextureData tdata; tdata.pSubResources = &srd; tdata.NumSubresources = 1;
		D::RefCntAutoPtr<D::ITexture> tex; dev->CreateTexture(td, &tdata, &tex);
		if (tex) { auto* srv = tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE); srv->AddRef(); result.atlasSRV = srv; }
	}

	FT_Done_Face(face); FT_Done_FreeType(ft);
	EInfo("Font[res]: {} ({}px, {}x{}, {} glyphs)", fontPath.path.string(), fontSize, atlasW, atlasH, result.glyphs.size());
	return result;
}

EE_NAMESPACE_UTILITIES_END
