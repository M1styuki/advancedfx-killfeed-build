#define NOMINMAX
#include "stdafx.h"
#include "MirvWeatherStorm.h"
#include "RenderSystemDX11Hooks.h"
#include "../shared/AfxConsole.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <atomic>
namespace {
std::atomic<unsigned long long> renderCalls {0};
std::atomic<unsigned long long> drawEntries {0};
std::atomic<unsigned long long> draws {0};
std::atomic<unsigned long long> resolveDraws {0};
std::atomic<unsigned long long> rejectedTargets {0};
std::atomic<unsigned long long> latchedSkips {0};
std::atomic<long> lastError {0};
std::atomic<long> lastDrawError {0};
std::atomic<long> lastTextureError {0};
std::atomic<long> lastSrvError {0};
std::atomic<long> lastMapError {0};
std::atomic<long> lastResolveError {0};
std::atomic<long> lastSupportError {0}; // CheckFormatSupport HRESULT for a rejected format
std::atomic<int> resolveSupport {0};   // MSAA resolve support for the sampled format: -1 no, 0 n/a, 1 yes
std::atomic<int> sampleSupport {0};    // copy/sample format support: -1 no, 0 unknown, 1 yes
std::atomic<int> lastSamples {0};
std::atomic<int> lastFormat {0};        // resource format
std::atomic<int> lastViewFormat {0};    // typed RTV/SRV view format
std::atomic<int> lastCopyFormat {0};    // single-sample copy / resolve format
std::atomic<int> lastReject {0};
std::atomic<int> shaderReady {0};

// Reject reasons are plain small ids: the render thread publishes them without
// taking a lock, and the status text maps them to short names so a failing
// target can be identified without attaching a debugger.
enum RejectReason {
    RejectReason_None = 0,
    RejectReason_NoTarget,
    RejectReason_NotTexture2D,
    RejectReason_Format,
    RejectReason_View,
    RejectReason_ArrayOrMip,
    RejectReason_Samples,
    RejectReason_ResolveSupport,
    RejectReason_SampleFormat,
    RejectReason_Device,
    RejectReason_Texture,
    RejectReason_Srv,
    RejectReason_Map,
    RejectReason_Resolve,
    RejectReason_Draw
};

const char * RejectReasonName(int value) {
    switch(value) {
    case RejectReason_None: return "none";
    case RejectReason_NoTarget: return "no-target";
    case RejectReason_NotTexture2D: return "not-texture2d";
    case RejectReason_Format: return "format";
    case RejectReason_View: return "view";
    case RejectReason_ArrayOrMip: return "array-or-mip";
    case RejectReason_Samples: return "samples";
    case RejectReason_ResolveSupport: return "resolve-support";
    case RejectReason_SampleFormat: return "sample-format";
    case RejectReason_Device: return "device";
    case RejectReason_Texture: return "texture";
    case RejectReason_Srv: return "srv";
    case RejectReason_Map: return "map";
    case RejectReason_Resolve: return "resolve";
    case RejectReason_Draw: return "draw";
    default: return "unknown";
    }
}

const char * FormatName(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    default: return "other";
    }
}

bool IsTypeless(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return true;
    default:
        return false;
    }
}

// A target signature identifies everything that decides whether the isolated
// pass can run. A deterministic rejection is latched per signature so an
// unsupported MSAA/format combination is diagnosed once instead of every frame.
struct TargetKey {
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    D3D11_RTV_DIMENSION dimension = D3D11_RTV_DIMENSION_UNKNOWN;
    UINT width = 0, height = 0;
    UINT samples = 0, arraySize = 0, mipLevels = 0;
    bool operator==(const TargetKey & other) const {
        return resourceFormat == other.resourceFormat && viewFormat == other.viewFormat
            && dimension == other.dimension && width == other.width && height == other.height
            && samples == other.samples && arraySize == other.arraySize && mipLevels == other.mipLevels;
    }
};

TargetKey MakeTargetKey(const D3D11_TEXTURE2D_DESC & desc, const D3D11_RENDER_TARGET_VIEW_DESC & view) {
    TargetKey key;
    key.resourceFormat = desc.Format;
    key.viewFormat = view.Format;
    key.dimension = view.ViewDimension;
    key.width = desc.Width;
    key.height = desc.Height;
    key.samples = desc.SampleDesc.Count;
    key.arraySize = desc.ArraySize;
    key.mipLevels = desc.MipLevels;
    return key;
}

template<class T> void Drop(T *& p) { if(p) p->Release(); p=nullptr; }
const char * shader=R"(
Texture2D image:register(t0); SamplerState samp:register(s0);
cbuffer Grade:register(b0) {float4 a;float4 b;}
struct V {float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID) {V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 PS(V v):SV_Target {
 float4 src=image.Sample(samp,v.uv);float3 c=max(src.rgb,0);
 if(max(max(c.r,c.g),c.b)<.0005)return src;
 // Preserve a fully white native flash even if its optional schema is absent.
 float white=min(min(src.r,src.g),src.b);
 float3 exposed=c*exp2(a.x);
 float lum=dot(exposed,float3(.2126,.7152,.0722));
 exposed=lerp(lum.xxx,exposed,a.y);
 float h=saturate((lum-.35)/.65);
 exposed*=1-a.z*h*h;
 exposed*=float3(1-a.w,1,1+a.w);
 exposed+=b.x*pow(saturate(1-lum),3)*smoothstep(.002,.02,lum);
 float protect=smoothstep(.96,1,white);
 return float4(lerp(exposed,src.rgb,protect),src.a);
})";

// The grading pass renders through its own deferred context and is executed on
// the immediate context with RestoreContextState=TRUE, which makes the runtime
// save and restore the target context state around the command list (the same
// mechanism HLAE's campath and depth-compositor passes already rely on) instead
// of a hand-written backup that could miss a stage or hazard-unbound slot.
// Recording into a deferred context also keeps HLAE's OMSetRenderTargets /
// OMSetBlendState / OMSetDepthStencilState blockers from mistaking the owned
// state for the game's state (deferred contexts use their own implementations,
// and the render-thread caller additionally marks the window with g_bInOwnDraw).
struct Drawer {
    ID3D11Device * device=nullptr;
    ID3D11DeviceContext * deferred=nullptr;
    ID3D11VertexShader * vs=nullptr;ID3D11PixelShader * ps=nullptr;
    ID3D11Buffer * cb=nullptr;ID3D11SamplerState * sampler=nullptr;ID3D11BlendState * blend=nullptr;
    ID3D11DepthStencilState * depth=nullptr;ID3D11RasterizerState * raster=nullptr;
    ID3D11Texture2D * copy=nullptr;ID3D11ShaderResourceView * srv=nullptr;
    D3D11_TEXTURE2D_DESC cached {}; DXGI_FORMAT viewFormat=DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT cachedSingleFormat=DXGI_FORMAT_UNKNOWN;
    bool refused=false;
    bool stopped=false;
    bool publishedDeviceReject=false;
    // Per-target latch. Only the render thread reads or writes it; the atomics
    // above are the cross-thread snapshot published for the status command.
    TargetKey latchKey {}; int latchReason=RejectReason_None;
    // Bounded set of already-diagnosed signatures so two alternating rejected
    // targets cannot turn the one-time warning into per-frame console spam.
    static const unsigned kWarnedCapacity=4;
    TargetKey warnedKeys[kWarnedCapacity] {}; int warnedReasons[kWarnedCapacity] {};
    unsigned warnedCount=0;
    void Clear() {
        Drop(deferred);Drop(srv);Drop(copy);Drop(vs);Drop(ps);Drop(cb);Drop(sampler);Drop(blend);Drop(depth);Drop(raster);
        Drop(device);refused=false;stopped=false;publishedDeviceReject=false;
        latchKey={};latchReason=RejectReason_None;
        for(unsigned i=0;i<kWarnedCapacity;++i){warnedKeys[i]={};warnedReasons[i]=RejectReason_None;}
        warnedCount=0;
        cached={};viewFormat=DXGI_FORMAT_UNKNOWN;cachedSingleFormat=DXGI_FORMAT_UNKNOWN;
        shaderReady.store(0);lastReject.store(RejectReason_None);
        lastError.store(0);lastDrawError.store(0);lastSupportError.store(0);
    }
    bool WasWarned(const TargetKey & key,int reason) const {
        for(unsigned i=0;i<warnedCount;++i) if(warnedReasons[i]==reason && warnedKeys[i]==key) return true;
        return false;
    }
    void RememberWarned(const TargetKey & key,int reason) {
        if(warnedCount<kWarnedCapacity) {
            warnedKeys[warnedCount]=key;warnedReasons[warnedCount]=reason;++warnedCount;
            return;
        }
        for(unsigned i=1;i<kWarnedCapacity;++i){warnedKeys[i-1]=warnedKeys[i];warnedReasons[i-1]=warnedReasons[i];}
        warnedKeys[kWarnedCapacity-1]=key;warnedReasons[kWarnedCapacity-1]=reason;
    }
    ~Drawer(){Clear();}
    bool Init(ID3D11Device * d) {
        if(!d)return false;
        if(device!=d){Clear();device=d;device->AddRef();}
        if(vs&&ps&&cb&&sampler&&blend&&depth&&raster&&deferred)return true;
        if(refused)return false;
        refused=true;
        // Deferred contexts are required for the isolated pass. Fail closed
        // (no grading) instead of touching the immediate context state by hand.
        if(FAILED(d->CreateDeferredContext(0,&deferred)) || !deferred) {
            lastError.store(E_FAIL);shaderReady.store(-1);return false;
        }
        ID3DBlob * v=nullptr,*p=nullptr,*error=nullptr;
        HRESULT h=D3DCompile(shader,strlen(shader),"mirv_weather_storm",nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&v,&error);Drop(error);
        if(SUCCEEDED(h))h=D3DCompile(shader,strlen(shader),"mirv_weather_storm",nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&p,&error);Drop(error);
        if(SUCCEEDED(h))h=device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs);
        if(SUCCEEDED(h))h=device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps);
        Drop(v);Drop(p);
        D3D11_BUFFER_DESC bd {};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(SUCCEEDED(h))h=device->CreateBuffer(&bd,nullptr,&cb);
        D3D11_SAMPLER_DESC sd {};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;sd.ComparisonFunc=D3D11_COMPARISON_NEVER;
        if(SUCCEEDED(h))h=device->CreateSamplerState(&sd,&sampler);
        D3D11_BLEND_DESC bl {};for(auto & rt:bl.RenderTarget){rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;}bl.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(SUCCEEDED(h))h=device->CreateBlendState(&bl,&blend);
        D3D11_DEPTH_STENCIL_DESC ds {};ds.DepthEnable=FALSE;ds.StencilEnable=FALSE;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilFunc=ds.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=ds.BackFace.StencilFailOp=ds.BackFace.StencilDepthFailOp=ds.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
        if(SUCCEEDED(h))h=device->CreateDepthStencilState(&ds,&depth);
        D3D11_RASTERIZER_DESC rs {};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;rs.MultisampleEnable=TRUE;
        if(SUCCEEDED(h))h=device->CreateRasterizerState(&rs,&raster);
        if(FAILED(h)){lastError.store(h);shaderReady.store(-1);return false;}
        shaderReady.store(1);
        return true;
    }
    // Publish the inspected target signature for the status snapshot. The
    // MSAA decision and the format checks are properties of this signature.
    void PublishTarget(const D3D11_TEXTURE2D_DESC & desc, const D3D11_RENDER_TARGET_VIEW_DESC & view, DXGI_FORMAT singleFormat) {
        lastSamples.store(static_cast<int>(desc.SampleDesc.Count));
        lastFormat.store(static_cast<int>(desc.Format));
        lastViewFormat.store(static_cast<int>(view.Format));
        lastCopyFormat.store(static_cast<int>(singleFormat));
    }
    void Reject(const D3D11_TEXTURE2D_DESC & desc, const D3D11_RENDER_TARGET_VIEW_DESC & view, int reason, bool latch) {
        lastReject.store(reason);
        rejectedTargets.fetch_add(1);
        if(!latch) return;
        const TargetKey key=MakeTargetKey(desc,view);
        if(latchReason==reason && latchKey==key) return;
        latchReason=reason;latchKey=key;
        if(WasWarned(key,reason)) return;
        RememberWarned(key,reason);
        // Publish diagnostics only. Engine-console output is confined to the
        // explicit status command on the engine thread.
    }
    void Draw(ID3D11DeviceContext * c,const MirvStormGrade & g) {
        // Read the current target without modifying any immediate-context state.
        drawEntries.fetch_add(1);
        ID3D11RenderTargetView * rtv=nullptr;
        c->OMGetRenderTargets(1,&rtv,nullptr);
        if(!rtv){lastReject.store(RejectReason_NoTarget);rejectedTargets.fetch_add(1);return;}
        ID3D11Resource * resource=nullptr;rtv->GetResource(&resource);
        ID3D11Texture2D * target=nullptr;
        if(resource)resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void **>(&target));Drop(resource);
        if(!target){lastReject.store(RejectReason_NotTexture2D);rejectedTargets.fetch_add(1);rtv->Release();return;}
        D3D11_TEXTURE2D_DESC desc {};target->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC rd {};rtv->GetDesc(&rd);
        const bool msaa=desc.SampleDesc.Count>1;
        // The resolve/copy needs a typed format. An RTV view format is always
        // typed, so a typeless MSAA resource is resolved through its view format
        // instead of guessing a resource format the runtime would reject.
        const DXGI_FORMAT singleFormat=msaa
            ? (IsTypeless(desc.Format)?rd.Format:desc.Format)
            : desc.Format;
        PublishTarget(desc,rd,singleFormat);
        ID3D11Device * inspectedDevice=nullptr;c->GetDevice(&inspectedDevice);
        if(!inspectedDevice){Reject(desc,rd,RejectReason_Device,false);Drop(target);rtv->Release();return;}
        if(device!=inspectedDevice) {
            Clear();device=inspectedDevice;device->AddRef();
            PublishTarget(desc,rd,singleFormat);
        }
        Drop(inspectedDevice);
        const TargetKey key=MakeTargetKey(desc,rd);
        if(latchReason!=RejectReason_None && latchKey==key) {
            latchedSkips.fetch_add(1);Drop(target);rtv->Release();return;
        }
        switch(rd.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT: break;
        default:Reject(desc,rd,RejectReason_Format,true);Drop(target);rtv->Release();return; // never grade depth/data/cubemap captures
        }
        // MSAA scene colour is a TEXTURE2DMS view; it is resolved to a
        // single-sample texture for sampling and the fullscreen pass still draws
        // back into the original MSAA render target.
        const bool viewOk=(!msaa && rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)
            || (msaa && (rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DMS || rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY));
        if(!viewOk){Reject(desc,rd,RejectReason_View,true);Drop(target);rtv->Release();return;}
        if(desc.ArraySize!=1 || desc.MipLevels!=1){Reject(desc,rd,RejectReason_ArrayOrMip,true);Drop(target);rtv->Release();return;}
        if(desc.SampleDesc.Count<1 || desc.SampleDesc.Count>16){Reject(desc,rd,RejectReason_Samples,true);Drop(target);rtv->Release();return;}
        ID3D11Device * d=nullptr;c->GetDevice(&d);
        if(!d){Reject(desc,rd,RejectReason_Device,false);Drop(target);rtv->Release();return;}
        UINT support=0;
        if(msaa) {
            // ResolveSubresource is only valid when the sampled format supports
            // multisample resolve on this device.
            resolveSupport.store(0);
            const HRESULT supported=d->CheckFormatSupport(singleFormat,&support);
            if(FAILED(supported)) lastSupportError.store(supported);
            if(FAILED(supported) || 0==(support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
                resolveSupport.store(-1);Reject(desc,rd,RejectReason_ResolveSupport,true);Drop(d);Drop(target);rtv->Release();return;
            }
            resolveSupport.store(1);
        } else resolveSupport.store(0);
        sampleSupport.store(0);
        // The single-sample texture must be creatable and the typed view format
        // must be sampleable; otherwise the deferred pixel shader cannot read it.
        HRESULT supported=d->CheckFormatSupport(singleFormat,&support);
        if(FAILED(supported)) lastSupportError.store(supported);
        if(FAILED(supported) || 0==(support&D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
            sampleSupport.store(-1);Reject(desc,rd,RejectReason_SampleFormat,true);Drop(d);Drop(target);rtv->Release();return;
        }
        supported=d->CheckFormatSupport(rd.Format,&support);
        if(FAILED(supported)) lastSupportError.store(supported);
        if(FAILED(supported) || 0==(support&D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
            sampleSupport.store(-1);Reject(desc,rd,RejectReason_SampleFormat,true);Drop(d);Drop(target);rtv->Release();return;
        }
        sampleSupport.store(1);
        const bool ready=Init(d);Drop(d);
        if(!ready || stopped) {
            // A stopped pass already published its own resolve/draw failure; do
            // not relabel it as a device failure.
            if(!ready && !publishedDeviceReject){publishedDeviceReject=true;Reject(desc,rd,RejectReason_Device,false);}
            Drop(target);rtv->Release();return;
        }
        if(!copy || !srv || cached.Width!=desc.Width || cached.Height!=desc.Height || cached.Format!=desc.Format
            || cached.SampleDesc.Count!=desc.SampleDesc.Count || viewFormat!=rd.Format || cachedSingleFormat!=singleFormat) {
            Drop(srv);Drop(copy);cached=desc;viewFormat=rd.Format;cachedSingleFormat=singleFormat;
            D3D11_TEXTURE2D_DESC single=desc;
            single.Format=singleFormat;single.SampleDesc.Count=1;single.SampleDesc.Quality=0;
            single.ArraySize=1;single.MipLevels=1;
            single.Usage=D3D11_USAGE_DEFAULT;single.BindFlags=D3D11_BIND_SHADER_RESOURCE;single.CPUAccessFlags=0;single.MiscFlags=0;
            const HRESULT created=device->CreateTexture2D(&single,nullptr,&copy);
            if(FAILED(created)) {
                lastTextureError.store(created);Reject(desc,rd,RejectReason_Texture,false);Drop(target);rtv->Release();return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC vd {};vd.Format=rd.Format;vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D.MipLevels=1;
            const HRESULT viewed=device->CreateShaderResourceView(copy,&vd,&srv);
            if(FAILED(viewed)) {
                lastSrvError.store(viewed);Reject(desc,rd,RejectReason_Srv,false);Drop(copy);Drop(target);rtv->Release();return;
            }
        }
        // The immediate context may be predicated. Resolve/CopyResource and
        // the owned draw honour predication; Map/Unmap are state-neutral.
        // Clear predication for the whole resolve+copy+execute
        // window and restore the exact previous state afterwards.
        ID3D11Predicate * predicate=nullptr;BOOL predicateValue=FALSE;
        c->GetPredication(&predicate,&predicateValue);
        if(predicate)c->SetPredication(nullptr,FALSE);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT mappedResult=c->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        if(FAILED(mappedResult)) {
            lastMapError.store(mappedResult);Reject(desc,rd,RejectReason_Map,false);
            if(predicate){c->SetPredication(predicate,predicateValue);predicate->Release();}
            Drop(target);rtv->Release();return;
        }
        float values[8]={g.exposure,g.saturation,g.highlight,g.cool,g.shadows,0,0,0};memcpy(mapped.pData,values,sizeof(values));c->Unmap(cb,0);
        lastReject.store(RejectReason_None);
        if(msaa) {
            c->ResolveSubresource(copy,0,target,0,singleFormat);
            // ResolveSubresource returns void, so a device-lost state is the
            // only failure signal a synchronous caller can observe.
            const HRESULT removed=device->GetDeviceRemovedReason();
            if(FAILED(removed)) {
                lastResolveError.store(removed);Reject(desc,rd,RejectReason_Resolve,true);stopped=true;
                if(predicate){c->SetPredication(predicate,predicateValue);predicate->Release();}
                Drop(target);rtv->Release();return;
            }
        } else {
            c->CopyResource(copy,target);
        }
        Drop(target);
        D3D11_VIEWPORT port {0,0,static_cast<float>(cached.Width),static_cast<float>(cached.Height),0,1};
        deferred->OMSetRenderTargets(1,&rtv,nullptr);
        deferred->OMSetBlendState(blend,nullptr,0xffffffff);
        deferred->OMSetDepthStencilState(depth,0);
        deferred->RSSetState(raster);
        deferred->RSSetViewports(1,&port);
        deferred->IASetInputLayout(nullptr);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->VSSetShader(vs,nullptr,0);
        deferred->PSSetShader(ps,nullptr,0);
        deferred->GSSetShader(nullptr,nullptr,0);
        deferred->HSSetShader(nullptr,nullptr,0);
        deferred->DSSetShader(nullptr,nullptr,0);
        deferred->PSSetShaderResources(0,1,&srv);
        deferred->PSSetSamplers(0,1,&sampler);
        deferred->PSSetConstantBuffers(0,1,&cb);
        deferred->Draw(3,0);
        ID3D11CommandList * list=nullptr;
        const HRESULT finish=deferred->FinishCommandList(FALSE,&list);
        if(SUCCEEDED(finish) && list) {
            c->ExecuteCommandList(list,TRUE); // saves and restores the immediate-context state
            draws.fetch_add(1);
            if(msaa)resolveDraws.fetch_add(1);
            lastReject.store(RejectReason_None);
        } else {
            // Fail closed: a command list that cannot be closed would be merged
            // into the next recording, so stop grading for this device instead.
            lastDrawError.store(SUCCEEDED(finish)?E_POINTER:finish);
            Reject(desc,rd,RejectReason_Draw,false);
            stopped=true;
        }
        if(list)list->Release();
        if(predicate){c->SetPredication(predicate,predicateValue);predicate->Release();}
        rtv->Release();
    }
} drawer;
}
void MirvWeatherStorm_Render(ID3D11DeviceContext * context) {
    const auto params=MirvWeatherStorm_Grade();if(context && params.active) {
        renderCalls.fetch_add(1);
        // A POV death fade must survive as a transition, not only as exact
        // black. The grade (and native exposure, restored by the controller) is
        // skipped for the whole fade so the fade colour ramp is untouched.
        if(RenderSystemDX11_DeathFade_IsActive()) return;
        drawer.Draw(context,params);
    }
}
void MirvWeatherStorm_RenderStatus() {
    // Counters prove that the pass was attempted, not that the result is
    // visible; a target can still be rejected before the shader ever runs.
    advancedfx::Message("[mirv_weather] grade shader=%d calls=%llu entries=%llu draws=%llu msaa=%llu latched=%llu rejected=%llu reject=%s samples=%d resource=%s(0x%x) view=%s(0x%x) copy=%s(0x%x) support=resolve:%d sample:%d.\n",
        shaderReady.load(),renderCalls.load(),drawEntries.load(),draws.load(),resolveDraws.load(),latchedSkips.load(),rejectedTargets.load(),
        RejectReasonName(lastReject.load()),lastSamples.load(),
        FormatName(static_cast<DXGI_FORMAT>(lastFormat.load())),static_cast<unsigned>(lastFormat.load()),
        FormatName(static_cast<DXGI_FORMAT>(lastViewFormat.load())),static_cast<unsigned>(lastViewFormat.load()),
        FormatName(static_cast<DXGI_FORMAT>(lastCopyFormat.load())),static_cast<unsigned>(lastCopyFormat.load()),
        resolveSupport.load(),sampleSupport.load());
    advancedfx::Message("[mirv_weather] grade errors error=0x%08x drawerror=0x%08x texture=0x%08x srv=0x%08x map=0x%08x resolve=0x%08x support=0x%08x (isolated before-HUD pass; no after-HUD fallback).\n",
        static_cast<unsigned>(lastError.load()),static_cast<unsigned>(lastDrawError.load()),
        static_cast<unsigned>(lastTextureError.load()),static_cast<unsigned>(lastSrvError.load()),
        static_cast<unsigned>(lastMapError.load()),static_cast<unsigned>(lastResolveError.load()),
        static_cast<unsigned>(lastSupportError.load()));
}
