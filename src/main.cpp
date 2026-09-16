
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <shlwapi.h>
#include <psapi.h>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")


// Local declaration of the Direct3D surface interop interface.
// Some Windows SDK / C++/WinRT combinations do not expose the symbol directly.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace D3DWinRT = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace DX = winrt::Windows::Graphics::DirectX;

static HWND g_overlay = nullptr;
static HWND g_roblox = nullptr;
static std::atomic<bool> g_running{ true };
static std::atomic<int> g_preset{ 0 }; // 0 day, 1 dusk, 2 dawn, 3 night
static std::atomic<float> g_strength{ 1.0f };

static ComPtr<ID3D11Device> g_device;
static ComPtr<ID3D11DeviceContext> g_context;
static ComPtr<IDXGISwapChain1> g_swapChain;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11SamplerState> g_sampler;
static ComPtr<ID3D11Buffer> g_cb;
static D3DWinRT::IDirect3DDevice g_winrtDevice{ nullptr };

static WGC::GraphicsCaptureItem g_item{ nullptr };
static WGC::Direct3D11CaptureFramePool g_framePool{ nullptr };
static WGC::GraphicsCaptureSession g_session{ nullptr };

static std::mutex g_renderMutex;
static SIZE g_swapSize{ 0,0 };

struct ShaderParams
{
    float resolution[2];
    float time;
    float strength;
    float preset;
    float pad[3];
};

static const char* kShader = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

cbuffer Params : register(b0)
{
    float2 resolution;
    float time;
    float strength;
    float preset;
    float3 _pad;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}

float lum(float3 c) { return dot(c, float3(0.2126,0.7152,0.0722)); }

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float3 filmic(float3 x)
{
    x = max(0, x - 0.004);
    return (x * (6.2*x + 0.5)) / (x*(6.2*x + 1.7) + 0.06);
}

float3 sampleClamp(float2 uv)
{
    return tex0.SampleLevel(samp0, saturate(uv), 0).rgb;
}

float4 PS(VSOut i) : SV_TARGET
{
    float2 uv = i.uv;
    float2 px = 1.0 / max(resolution, float2(1,1));
    float3 base = sampleClamp(uv);
    float L = lum(base);

    // tiny diffusion / local contrast
    float3 blur = 0;
    blur += sampleClamp(uv + float2(px.x*2,0));
    blur += sampleClamp(uv - float2(px.x*2,0));
    blur += sampleClamp(uv + float2(0,px.y*2));
    blur += sampleClamp(uv - float2(0,px.y*2));
    blur *= 0.25;

    // bright bloom ring
    float3 bloom = 0;
    [unroll] for (int k=0;k<8;k++)
    {
        float a = 6.2831853 * (k/8.0);
        float2 off = float2(cos(a),sin(a)) * px * 6.0;
        float3 s = sampleClamp(uv + off);
        float b = smoothstep(0.72, 1.05, lum(s));
        bloom += s * b;
    }
    bloom *= 0.125;

    // light shafts from bright pixels, down-screen by default
    float2 rayDir = normalize(float2(0.22, 0.12));
    float3 rays = 0;
    [unroll] for (int r=1;r<=8;r++)
    {
        float t = r / 8.0;
        float3 s = sampleClamp(uv - rayDir * (0.14*t));
        float b = smoothstep(0.74, 1.03, lum(s));
        rays += s * b * (1.0-t);
    }
    rays *= 0.10 * (1.10 - smoothstep(0.45,0.95,L));

    // horizon haze / atmospheric perspective approximation
    float horizon = 0.56;
    float h = 1.0 - smoothstep(0.07, 0.34, abs(uv.y - horizon));
    float detail = saturate(length(base - blur) * 4.0);
    float fogMask = h * (1.0 - 0.55*detail);

    // infer scene tint from local image
    float3 fogColor = lerp(float3(0.58,0.66,0.78), max(base,blur), 0.55);

    float bloomAmt = 0.58;
    float raysAmt = 0.72;
    float fogAmt = 0.29;
    float shadowAmt = 0.18;
    float warmth = 0.08;
    float sat = 1.06;

    if (preset > 0.5 && preset < 1.5) { // Golden Dusk
        bloomAmt=0.68; raysAmt=0.92; fogAmt=0.36; warmth=0.28; sat=1.10;
        fogColor *= float3(1.16,1.02,0.90);
    }
    else if (preset >= 1.5 && preset < 2.5) { // Misty Dawn
        bloomAmt=0.54; raysAmt=0.62; fogAmt=0.43; warmth=0.12; sat=0.98;
        fogColor = lerp(fogColor, float3(0.88,0.78,0.88), 0.24);
    }
    else if (preset >= 2.5) { // Ethereal Night
        bloomAmt=0.48; raysAmt=0.16; fogAmt=0.18; warmth=-0.18; sat=0.92;
        fogColor = lerp(fogColor, float3(0.22,0.30,0.48), 0.55);
    }

    // pseudo-contact shadow shaping
    float localL = lum(blur);
    float shadow = saturate((localL-L)*2.2 + (1.0-smoothstep(0.10,0.52,L))*0.25);

    float3 c = lerp(base, blur, 0.12);
    c *= 1.0 - shadow * shadowAmt * 0.42;
    c += bloom * bloomAmt;
    c += rays * raysAmt;
    c = lerp(c, fogColor, saturate(fogMask*fogAmt));

    // warm highlights / cool shadows
    float hi = smoothstep(0.45,1.0,lum(c));
    c.r += max(warmth,0.0)*0.065*hi;
    c.b -= max(warmth,0.0)*0.050*hi;
    c.b += max(-warmth,0.0)*0.055*(1.0-hi);

    float l2 = lum(c);
    c = lerp(l2.xxx, c, sat);

    // subtle procedural night stars in dark upper-screen areas only
    if (preset >= 2.5)
    {
        float skyMask = (1.0-smoothstep(0.18,0.52,uv.y)) * (1.0-smoothstep(0.08,0.32,L));
        float2 cell = floor(uv * resolution / 5.0);
        float n = hash21(cell);
        float star = smoothstep(0.996, 1.0, n) * skyMask;
        float band = exp(-pow((uv.y - (0.22 + 0.10*sin(uv.x*5.2))) / 0.11, 2.0));
        float dust = hash21(floor(uv*resolution/18.0)) * band * skyMask;
        c += star * float3(0.9,0.95,1.0) * 0.55;
        c += dust * float3(0.12,0.15,0.22) * 0.22;
    }

    c *= 1.05;
    c = filmic(c);

    // vignette
    float2 p = uv*2-1;
    float vig = smoothstep(1.25,0.20,dot(p,p));
    c *= lerp(0.94,1.0,vig);

    c = lerp(base, c, strength);
    return float4(saturate(c), 1);
}
)";

static std::wstring GetProcessPath(DWORD pid)
{
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH * 4]{};
    DWORD sz = (DWORD)_countof(buf);
    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) out.assign(buf, sz);
    CloseHandle(h);
    return out;
}

static BOOL CALLBACK FindRobloxProc(HWND hwnd, LPARAM)
{
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto path = GetProcessPath(pid);
    if (path.empty()) return TRUE;

    auto name = PathFindFileNameW(path.c_str());
    if (_wcsicmp(name, L"RobloxPlayerBeta.exe") == 0)
    {
        RECT r{};
        GetWindowRect(hwnd, &r);
        if ((r.right-r.left) > 400 && (r.bottom-r.top) > 300)
        {
            g_roblox = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND FindRobloxWindow()
{
    g_roblox = nullptr;
    EnumWindows(FindRobloxProc, 0);
    return g_roblox;
}

static D3DWinRT::IDirect3DDevice CreateWinRTDevice(ID3D11Device* dev)
{
    ComPtr<IDXGIDevice> dxgi;
    winrt::check_hresult(dev->QueryInterface(IID_PPV_ARGS(&dxgi)));

    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
    return inspectable.as<D3DWinRT::IDirect3DDevice>();
}

static ComPtr<ID3D11Texture2D> GetTextureFromSurface(D3DWinRT::IDirect3DSurface const& surface)
{
    auto access = surface.as<IDirect3DDxgiInterfaceAccess>();

    ComPtr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(tex.GetAddressOf())
    ));
    return tex;
}

static WGC::GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd)
{
    auto factory = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(
        hwnd,
        winrt::guid_of<WGC::GraphicsCaptureItem>(),
        winrt::put_abi(item)
    ));
    return item;
}

static bool CompileShaders()
{
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
        "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &err);
    if (FAILED(hr)) return false;
    hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
        "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &err);
    if (FAILED(hr)) return false;

    if (FAILED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs))) return false;
    if (FAILED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(g_device->CreateSamplerState(&sd, &g_sampler))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_cb))) return false;

    return true;
}

static bool CreateSwapChain(int w, int h)
{
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(g_device.As(&dxgiDev))) return false;
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        g_device.Get(), g_overlay, &desc, nullptr, nullptr, &g_swapChain);
    if (FAILED(hr)) return false;
    g_swapSize = { w,h };
    return true;
}

static bool EnsureSwapSize(int w, int h)
{
    if (!g_swapChain) return CreateSwapChain(w,h);
    if (g_swapSize.cx == w && g_swapSize.cy == h) return true;

    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    HRESULT hr = g_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return false;
    g_swapSize = { w,h };
    return true;
}

static void RenderFrame(ID3D11Texture2D* frameTex)
{
    std::scoped_lock lk(g_renderMutex);
    D3D11_TEXTURE2D_DESC td{};
    frameTex->GetDesc(&td);
    if (td.Width < 2 || td.Height < 2) return;
    if (!EnsureSwapSize((int)td.Width, (int)td.Height)) return;

    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(frameTex, &sv, &srv))) return;

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_device->CreateRenderTargetView(back.Get(), nullptr, &rtv))) return;

    D3D11_VIEWPORT vp{};
    vp.Width = (float)td.Width;
    vp.Height = (float)td.Height;
    vp.MaxDepth = 1.0f;
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

    LARGE_INTEGER qpf{}, qpc{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc);

    ShaderParams p{};
    p.resolution[0] = (float)td.Width;
    p.resolution[1] = (float)td.Height;
    p.time = (float)((double)qpc.QuadPart / (double)qpf.QuadPart);
    p.strength = g_strength.load();
    p.preset = (float)g_preset.load();

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(g_context->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        memcpy(map.pData, &p, sizeof(p));
        g_context->Unmap(g_cb.Get(), 0);
    }

    ID3D11ShaderResourceView* srvs[] = { srv.Get() };
    ID3D11SamplerState* samps[] = { g_sampler.Get() };
    ID3D11Buffer* cbs[] = { g_cb.Get() };

    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vs.Get(), nullptr, 0);
    g_context->PSSetShader(g_ps.Get(), nullptr, 0);
    g_context->PSSetShaderResources(0, 1, srvs);
    g_context->PSSetSamplers(0, 1, samps);
    g_context->PSSetConstantBuffers(0, 1, cbs);

    g_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[] = { nullptr };
    g_context->PSSetShaderResources(0, 1, nullSrv);

    g_swapChain->Present(1, 0);
}

static bool InitD3D()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        fls, _countof(fls), D3D11_SDK_VERSION,
        &g_device, &fl, &g_context);
    if (FAILED(hr)) return false;

    g_winrtDevice = CreateWinRTDevice(g_device.Get());
    return CompileShaders();
}

static void StartCapture()
{
    g_item = CreateCaptureItemForWindow(g_roblox);
    auto size = g_item.Size();

    g_framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
        g_winrtDevice,
        DX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size);

    g_session = g_framePool.CreateCaptureSession(g_item);

    g_framePool.FrameArrived([](auto const& pool, auto const&) {
        try
        {
            auto frame = pool.TryGetNextFrame();
            if (!frame) return;
            auto tex = GetTextureFromSurface(frame.Surface());
            RenderFrame(tex.Get());
        }
        catch (...) {}
    });

    g_session.StartCapture();
}

static void StopCapture()
{
    try {
        if (g_session) g_session.Close();
        if (g_framePool) g_framePool.Close();
    } catch (...) {}
    g_session = nullptr;
    g_framePool = nullptr;
    g_item = nullptr;
}

static void TrackRoblox()
{
    if (!g_roblox || !IsWindow(g_roblox))
    {
        g_running = false;
        PostMessage(g_overlay, WM_CLOSE, 0, 0);
        return;
    }

    RECT r{};
    if (IsIconic(g_roblox) || !GetWindowRect(g_roblox, &r))
    {
        ShowWindow(g_overlay, SW_HIDE);
        return;
    }

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
    {
        ShowWindow(g_overlay, SW_HIDE);
        return;
    }

    SetWindowPos(g_overlay, HWND_TOPMOST, r.left, r.top, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void UpdateTitle()
{
    const wchar_t* names[] = { L"Dreamy Day", L"Golden Dusk", L"Misty Dawn", L"Ethereal Night" };
    wchar_t buf[256]{};
    swprintf_s(buf, L"Ethereal v0.1 — %s — F8 preset | F9 toggle | +/- strength | Esc exit",
        names[g_preset.load()]);
    SetWindowTextW(g_overlay, buf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        TrackRoblox();
        return 0;
    case WM_HOTKEY:
        if (wp == 1) { // F8
            g_preset = (g_preset.load() + 1) % 4;
            UpdateTitle();
        }
        else if (wp == 2) { // F9
            float s = g_strength.load();
            g_strength = (s > 0.05f) ? 0.0f : 1.0f;
        }
        else if (wp == 3) { // +
            g_strength = std::min(1.6f, g_strength.load() + 0.1f);
        }
        else if (wp == 4) { // -
            g_strength = std::max(0.0f, g_strength.load() - 0.1f);
        }
        else if (wp == 5) { // Esc
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        g_running = false;
        KillTimer(hwnd, 1);
        UnregisterHotKey(hwnd, 1);
        UnregisterHotKey(hwnd, 2);
        UnregisterHotKey(hwnd, 3);
        UnregisterHotKey(hwnd, 4);
        UnregisterHotKey(hwnd, 5);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (!WGC::GraphicsCaptureSession::IsSupported())
    {
        MessageBoxW(nullptr, L"Windows Graphics Capture is not supported on this system.\nUse Windows 10 1903+ or Windows 11.", L"Ethereal", MB_ICONERROR);
        return 1;
    }

    if (!FindRobloxWindow())
    {
        MessageBoxW(nullptr,
            L"Roblox was not found.\n\nOpen Roblox, enter a game, then start Ethereal again.",
            L"Ethereal v0.1", MB_ICONINFORMATION);
        return 2;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"EtherealOverlayClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT r{};
    GetWindowRect(g_roblox, &r);

    DWORD ex = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_overlay = CreateWindowExW(
        ex, wc.lpszClassName, L"Ethereal v0.1",
        WS_POPUP,
        r.left, r.top, r.right-r.left, r.bottom-r.top,
        nullptr, nullptr, hInst, nullptr);

    if (!g_overlay)
        return 3;

    if (!InitD3D())
    {
        MessageBoxW(nullptr, L"Could not initialize Direct3D 11.", L"Ethereal", MB_ICONERROR);
        return 4;
    }

    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    SetWindowPos(g_overlay, HWND_TOPMOST, 0,0,0,0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    RegisterHotKey(g_overlay, 1, 0, VK_F8);
    RegisterHotKey(g_overlay, 2, 0, VK_F9);
    RegisterHotKey(g_overlay, 3, 0, VK_OEM_PLUS);
    RegisterHotKey(g_overlay, 4, 0, VK_OEM_MINUS);
    RegisterHotKey(g_overlay, 5, 0, VK_ESCAPE);

    SetTimer(g_overlay, 1, 250, nullptr);
    UpdateTitle();

    try
    {
        StartCapture();
    }
    catch (winrt::hresult_error const& e)
    {
        MessageBoxW(nullptr, e.message().c_str(), L"Capture failed", MB_ICONERROR);
        return 5;
    }

    MessageBoxW(nullptr,
        L"Ethereal is active.\n\n"
        L"F8  = change preset\n"
        L"F9  = toggle effect\n"
        L"+/- = strength\n"
        L"Esc = close Ethereal\n\n"
        L"The overlay is click-through, so your mouse and keyboard continue controlling Roblox.",
        L"Ethereal v0.1", MB_OK | MB_ICONINFORMATION);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopCapture();
    g_swapChain.Reset();
    g_context.Reset();
    g_device.Reset();
    return 0;
}
