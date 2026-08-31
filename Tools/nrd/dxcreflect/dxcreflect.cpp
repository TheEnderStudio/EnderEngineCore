// Reflect DXBC/DXIL containers using DXC's IDxcContainerReflection, printing
// shader resources (name/type/bind) - the reliable way to read RDAT.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <cstdio>
#include <cstdint>
#include <vector>

typedef HRESULT(WINAPI* PFN_DxcCreateInstance)(REFCLSID, REFIID, void**);

static const char* TypeName(D3D_SHADER_INPUT_TYPE t) {
    switch (t) {
    case D3D_SIT_CBUFFER: return "CBUFFER";
    case D3D_SIT_TBUFFER: return "TBUFFER";
    case D3D_SIT_TEXTURE: return "TEXTURE";
    case D3D_SIT_SAMPLER: return "SAMPLER";
    case D3D_SIT_UAV_RWTYPED: return "UAV_RWTYPED";
    case D3D_SIT_STRUCTURED: return "STRUCTURED";
    case D3D_SIT_UAV_RWSTRUCTURED: return "UAV_RWSTRUCTURED";
    case D3D_SIT_BYTEADDRESS: return "BYTEADDRESS";
    case D3D_SIT_UAV_RWBYTEADDRESS: return "UAV_RWBYTEADDRESS";
    case D3D_SIT_UAV_APPEND_STRUCTURED: return "UAV_APPEND_STRUCTURED";
    case D3D_SIT_UAV_CONSUME_STRUCTURED: return "UAV_CONSUME_STRUCTURED";
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: return "UAV_RWSTRUCTURED_WITH_COUNTER";
    case D3D_SIT_RTACCELERATIONSTRUCTURE: return "RTAS";
    case D3D_SIT_UAV_FEEDBACKTEXTURE: return "UAV_FEEDBACKTEXTURE";
    default: return "?";
    }
}

// Minimal IDxcBlob implementation wrapping external memory (old dxcapi.h Load takes IDxcBlob*).
struct BlobImpl : public IDxcBlob {
    const void* m_ptr;
    SIZE_T m_size;
    BlobImpl(const void* p, SIZE_T s) : m_ptr(p), m_size(s) {}
    STDMETHODIMP QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP_(LPVOID) GetBufferPointer() override { return (LPVOID)m_ptr; }
    STDMETHODIMP_(SIZE_T) GetBufferSize() override { return m_size; }
};

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: dxcreflect <shader.dxbc|dxil>\n"); return 1; }

    HMODULE dxc = LoadLibraryA("dxcompiler.dll");
    if (!dxc) { printf("cannot load dxcompiler.dll\n"); return 1; }
    auto fnCreate = (PFN_DxcCreateInstance)GetProcAddress(dxc, "DxcCreateInstance");
    if (!fnCreate) { printf("no DxcCreateInstance\n"); return 1; }

    IDxcContainerReflection* refl = nullptr;
    HRESULT hr = fnCreate(CLSID_DxcContainerReflection, __uuidof(IDxcContainerReflection), (void**)&refl);
    if (FAILED(hr)) { printf("create container reflection failed 0x%08X\n", (unsigned)hr); return 1; }

    FILE* f = nullptr;
    fopen_s(&f, argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(len);
    fread(data.data(), 1, len, f);
    fclose(f);

    BlobImpl blob(data.data(), data.size());
    HRESULT loadHr = refl->Load(&blob);
    if (FAILED(loadHr)) { printf("container Load failed 0x%08X\n", (unsigned)loadHr); return 1; }

    UINT32 partCount = 0;
    refl->GetPartCount(&partCount);
    printf("parts: %u\n", partCount);

    for (UINT32 i = 0; i < partCount; ++i) {
        UINT32 kind = 0;
        refl->GetPartKind(i, &kind);
        if (kind != DXC_PART_DXIL) continue; // DXIL program part

        ID3D11ShaderReflection* sr = nullptr;
        HRESULT rhr = refl->GetPartReflection(i, IID_ID3D11ShaderReflection, (void**)&sr);
        if (FAILED(rhr)) { printf("GetPartReflection failed 0x%08X\n", (unsigned)rhr); continue; }

        D3D11_SHADER_DESC sd{};
        sr->GetDesc(&sd);
        printf("=== shader: creator='%s' version=0x%X resources=%u ===\n",
            sd.Creator ? sd.Creator : "(null)", sd.Version, sd.BoundResources);
        for (UINT r = 0; r < sd.BoundResources; ++r) {
            D3D11_SHADER_INPUT_BIND_DESC b{};
            sr->GetResourceBindingDesc(r, &b);
            printf("  [%u] name='%s' type=%s bind=%u count=%u dim=%u\n",
                r, b.Name ? b.Name : "(null)", TypeName(b.Type), b.BindPoint, b.BindCount, (unsigned)b.Dimension);
        }
        sr->Release();
    }
    refl->Release();
    return 0;
}
