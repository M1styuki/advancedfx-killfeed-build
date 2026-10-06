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
std::atomic<unsigned long long> draws {0};
std::atomic<long> lastError {0};
std::atomic<long> lastDrawError {0};
std::atomic<int> shaderReady {0};
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
 exposed=lerp(exposed,float3(.92,.96,1),b.y);
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
    bool refused=false;
    bool stopped=false;
    void Clear() {
        Drop(deferred);Drop(srv);Drop(copy);Drop(vs);Drop(ps);Drop(cb);Drop(sampler);Drop(blend);Drop(depth);Drop(raster);
        Drop(device);refused=false;stopped=false;cached={};viewFormat=DXGI_FORMAT_UNKNOWN;
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
        D3D11_RASTERIZER_DESC rs {};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        if(SUCCEEDED(h))h=device->CreateRasterizerState(&rs,&raster);
        if(FAILED(h)){lastError.store(h);shaderReady.store(-1);return false;}
        shaderReady.store(1);
        return true;
    }
    void Draw(ID3D11DeviceContext * c,const MirvStormGrade & g) {
        // Read the current target without modifying any immediate-context state.
        ID3D11RenderTargetView * rtv=nullptr;
        c->OMGetRenderTargets(1,&rtv,nullptr);
        if(!rtv)return;
        ID3D11Resource * resource=nullptr;rtv->GetResource(&resource);
        ID3D11Texture2D * target=nullptr;
        if(resource)resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void **>(&target));Drop(resource);
        if(!target){rtv->Release();return;}
        D3D11_TEXTURE2D_DESC desc {};target->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC rd {};rtv->GetDesc(&rd);
        switch(rd.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT: break;
        default:Drop(target);rtv->Release();return; // never grade depth/data/cubemap captures
        }
        if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1 || rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D){Drop(target);rtv->Release();return;}
        ID3D11Device * d=nullptr;c->GetDevice(&d);const bool ready=Init(d);Drop(d);if(!ready || stopped){Drop(target);rtv->Release();return;}
        if(!copy || cached.Width!=desc.Width || cached.Height!=desc.Height || cached.Format!=desc.Format || viewFormat!=rd.Format) {
            Drop(srv);Drop(copy);cached=desc;viewFormat=rd.Format;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=0;desc.MiscFlags=0;
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&copy))){Drop(target);rtv->Release();return;}
            D3D11_SHADER_RESOURCE_VIEW_DESC vd {};vd.Format=rd.Format;vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D.MipLevels=1;
            if(FAILED(device->CreateShaderResourceView(copy,&vd,&srv))){Drop(copy);Drop(target);rtv->Release();return;}
        }
        // The immediate context may be predicated. Draw and CopyResource honour
        // predication, so a false predicate would skip the copy and/or the owned
        // draw. Clear predication for the whole copy+execute window and restore
        // the exact previous state afterwards. Map/Unmap are state-neutral and
        // are not predicated.
        ID3D11Predicate * predicate=nullptr;BOOL predicateValue=FALSE;
        c->GetPredication(&predicate,&predicateValue);
        if(predicate)c->SetPredication(nullptr,FALSE);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if(FAILED(c->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) {
            if(predicate){c->SetPredication(predicate,predicateValue);predicate->Release();}
            Drop(target);rtv->Release();return;
        }
        float values[8]={g.exposure,g.saturation,g.highlight,g.cool,g.shadows,g.flash,0,0};memcpy(mapped.pData,values,sizeof(values));c->Unmap(cb,0);
        c->CopyResource(copy,target);Drop(target);
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
        } else {
            // Fail closed: a command list that cannot be closed would be merged
            // into the next recording, so stop grading for this device instead.
            lastDrawError.store(SUCCEEDED(finish)?E_POINTER:finish);
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
        // A POV death fade must survive as a transition, not only as exact
        // black. The grade (and native exposure, restored by the controller) is
        // skipped for the whole fade so the fade colour ramp is untouched.
        if(RenderSystemDX11_DeathFade_IsActive()) return;
        drawer.Draw(context,params);
    }
}
void MirvWeatherStorm_RenderStatus() {
    advancedfx::Message("[mirv_weather] grade shader=%d draws=%llu error=0x%08x drawerror=0x%08x (isolated before-HUD pass).\n",
        shaderReady.load(),draws.load(),static_cast<unsigned>(lastError.load()),static_cast<unsigned>(lastDrawError.load()));
}
