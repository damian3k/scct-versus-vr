// OpenXR session, D3D12 interop through D3D9On12 and the alternate-eye frame loop.
//
// Frame pairing (alternate frame rendering):
//   game frame 2n   -> rendered with the left eye camera  -> copied into swapchain[0]
//   game frame 2n+1 -> rendered with the right eye camera -> copied into swapchain[1], xrEndFrame
// xrWaitFrame/xrBeginFrame/xrLocateViews run once per pair, so the game must run at about
// twice the headset refresh rate for full smoothness (the runtime reprojects otherwise).
//
// 2D drawing (HUD, menus, world markers) is redirected by the device hooks into hudTex and shown
// as a quad layer covering exactly the game camera's field of view, so markers that the game
// projected with its own camera line up with the stereo world.
//
// Aim mode: the rendered view turns with a "body yaw" driven by the thumbstick, while the game's
// own camera (and so the weapon) is steered by emulated mouse motion towards the right
// controller's aim ray.

#include "vr.h"
#include "vrmath.h"
#include "config.h"
#include "log.h"
#include "input.h"
#include "xrapi.h"
#include "rawinject.h"
#include "dihook.h"

#include <dxgi1_4.h>
#include <d3d9on12.h>
#include <vector>

namespace vr {

namespace {

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
};

struct State {
    bool initTried = false;
    bool ok = false;

    // WinlatorXR XR API backend (standalone headset) instead of OpenXR: no swapchains or layers,
    // the game window itself is shown, one eye per game frame (alternate-eye mode).
    bool xrapi = false;
    int xrSync = 0;             // HMD_SYNC of the pose this frame is rendered with
    int xrEyeNext = 0;
    unsigned xrSceneFrames = 0; // consecutive 3D gameplay frames
    int xrSentVr = -1, xrSent3D = -1;
    DWORD xrStateTick = 0;
    unsigned xrPackets = 0;
    bool xrLoggedFov = false;
    xrapi::Packet xrPacket = {};    // latest packet (controllers for the input layer)

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    uint32_t recommendedW = 0, recommendedH = 0;

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    static const int kAllocators = 4;
    ID3D12CommandAllocator* allocators[kAllocators] = {};
    UINT64 allocatorFence[kAllocators] = {};
    int nextAllocator = 0;
    ID3D12GraphicsCommandList* cmdList = nullptr;

    // D3D9 side
    IDirect3DDevice9* dev9 = nullptr;
    IDirect3DDevice9On12* dev9On12 = nullptr;
    IDirect3DTexture9* copyTex = nullptr;   // backbuffer is StretchRect'ed here, then unwrapped to D3D12
    IDirect3DTexture9* hudTex = nullptr;    // 2D draws are redirected here (premultiplied alpha)
    D3DFORMAT copyFormat = D3DFMT_A8R8G8B8;
    int64_t xrFormat = 0;
    UINT width = 0, height = 0;
    EyeSwapchain eyes[2];
    EyeSwapchain hud;
    EyeSwapchain laser;         // small solid-colour image for the menu laser beam / dot
    ID3D12DescriptorHeap* laserRtv = nullptr;
    DWORD focusTick = 0;
    // menu cursor (game virtual 2D space)
    bool cursorValid = false;
    float cursorX = 0, cursorY = 0, cursorSpaceW = 640, cursorSpaceH = 480;
    bool menuPrevValid = false;
    float menuPrevX = 0, menuPrevY = 0;
    int menuSentX = 0, menuSentY = 0;
    float menuCountsPerPx = 1.0f;
    float menuCalibCounts = 0, menuCalibPx = 0, menuCalibTime = 0;
    unsigned cursorStreak = 0;
    DWORD menuUiTick = 0;
    bool menuLatch = false;     // unused: the in-game menu is detected from its drawing now
    unsigned inGameMenuStreak = 0;
    DWORD inGameMenuTick = 0;
    bool lastMenuUi = false;
    bool hudDrawn = false;      // this game frame
    bool hudCopied = false;     // this pair
    bool hudDrawnPair = false;  // any frame of this pair drew HUD
    HWND gameWnd = nullptr;

    // Game camera as last seen this pair (Unreal world, radians)
    bool gameAnglesValid = false;
    bool anglesThisFrame = false;
    bool hudRelThisFrame = false, hudRelValid = false;
    bool hudFollow = false;      // current HUD mode (toggled in game)
    float hudDistance = 5.0f;    // current HUD distance
    float hudFollowScale = 0.45f; // current follow-mode panel size (adjusted in game with Num+ / Num-)
    bool hudNearKey = false, hudFarKey = false;
    bool hudKeyDown = false;
    Quat hudRel = { 0, 0, 0, 1 };
    float gameYaw = 0, gamePitch = 0;
    float gameTanX = 1, gameTanY = 1;
    bool sceneLastPair = false;

    // Controllers / aim
    input::Controls ctl;
    LARGE_INTEGER lastPairTime = {};
    bool aimOn = true;          // user toggle (End key / AimToggle binding)
    DWORD lastWeaponTick = 0;   // last frame a first-person weapon was drawn
    unsigned frameSceneDraws = 0;
    unsigned skipNoResources = 0, skipNoRender = 0, skipNoViews = 0, skipCopyFailed = 0;
    D3DMATRIX eyeProj[2];
    bool aimKeyDown = false;
    bool bodyValid = false;
    float bodyYaw = 0, prevGameYaw = 0;
    bool snapHeld = false;
    float countsPerDeg = 30.0f;         // learned mouse response (starts low: undershooting is safe)
    float remX = 0, remY = 0;
    float calibCounts = 0, calibDeg = 0, calibTime = 0;
    int lastSentX = 0;
    float aimPause = 0;         // seconds left without mouse output (game ignores input)
    float stallSentDeg = 0, stallSeenDeg = 0, stallTime = 0;
    int calibAccepted = 0;
    bool handValid = false;
    D3DMATRIX handMat;          // right controller -> body camera space (LH, Unreal units)
    bool baseValid = false;
    D3DMATRIX baseView;         // world -> body camera space used for rendering this frame

    // Merc zoom (binoculars / sniper mode)
    bool gameZoomed = false;    // the game camera's FOV is narrowed
    bool frameScope = false;    // this game frame is rendered flat with the game camera
    bool eyeScope[2] = {};
    bool normalFovValid = false;
    float normalTanX = 1, normalTanY = 1;
    bool scopeGestureOn = false;
    float scopeDwell = 0;
    bool scopeTapped = false;   // the gesture pressed the zoom key
    bool reloadIn = false;      // left hand at the magazine (re-armed when it leaves)
    float reloadDwell = 0;
    DWORD reloadTick = 0;       // last reload sent
    bool scopeSeen = false;     // the game zoomed during the gesture
    bool scopeAimLast = false;  // the previous aim update was zoomed
    bool scopeAimValid = false;
    float scopeAimYaw = 0, scopeAimPitch = 0;
    bool scopeHead = false;     // spy binoculars: the head looks around, not the controller
    int wideFrames = 0;

    unsigned scopePairs = 0;

    // Spy (third person): the spy model's origin, found among the draws (see HideDraw), gives
    // the spy's exact position. The VR view is placed relative to it:
    // - stabilised third person: where the game camera is relative to the spy (in the game
    //   camera's axes), but with the level body-yaw axes, so turning the camera does not swing it;
    // - first-person view (both stick clicks): the spy's eyes, an offset (in the level axes)
    //   that the binoculars measure - they put the game camera exactly at the spy's eyes.
    bool spyFpv = false;
    bool spyCamFrame = false;       // the spy's main camera was seen this frame
    Vec3 spyCamPos = { 0, 0, 0 }, spyCamFwd = { 0, 0, 1 };
    bool spyOriginValid = false;    // the spy model's origin (last frame)
    Vec3 spyOrigin = { 0, 0, 0 };
    unsigned spyBestPrims = 0;
    Vec3 spyBestOrigin = { 0, 0, 0 };
    bool spyHeadValid = false, spyBestHeadValid = false;    // top of the spy's head (vertices)
    Vec3 spyHead = { 0, 0, 0 }, spyBestHead = { 0, 0, 0 };
    bool loggedHead = false;
    Vec3 spyAxes[3];                // rendered level camera right, up, forward
    bool curCamMain = true;
    float eyeRight = 0, eyeUp = 0, eyeFwd = 0;
    DWORD zoomStartTick = 0;
    bool eyeCalibDone = false;
    unsigned hiddenDraws = 0;

    // Camera positions used this frame and their 3D draw counts. The one with the most draws is
    // the main view; others are e.g. the skybox camera (inside a small-scale sky zone model).
    static const int kCamSlots = 6;
    Vec3 camSlotPos[kCamSlots];
    unsigned camSlotDraws[kCamSlots] = {};
    int camSlotCount = 0, curCamSlot = -1;
    bool mainCamValid = false;
    Vec3 mainCamPos = { 0, 0, 0 };

    // Frame state
    bool frameOpen = false;
    int curEye = 0;
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    bool viewsValid = false;
    bool eyeCopied[2] = {};
    bool eyeScene[2] = {};
    D3DMATRIX eyeH[2];      // game camera space -> eye space

    // Recenter reference (yaw only), in LOCAL space
    bool recenterPending = true;
    Quat refQ = { 0, 0, 0, 1 };
    Vec3 refP = { 0, 0, 0 };
    bool recenterKeyDown = false;

    // Level-view fallback when looking straight up/down
    bool haveLastLevel = false;
    Vec3 lastFlatFwd = { 0, 0, 1 };

    // Stats
    DWORD statsStart = 0;
    unsigned presents = 0, pairs = 0, stereoViews = 0, quadPairs = 0, hudPairs = 0;
} S;

const float kDeg = 57.29578f;

float WrapPi(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

Quat AxisAngle(Vec3 axis, float angle)
{
    float s = sinf(angle * 0.5f);
    return { axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f) };
}

PFN_xrGetD3D12GraphicsRequirementsKHR pfnGetD3D12Reqs = nullptr;

bool XrOk(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (S.instance) xrResultToString(S.instance, r, buf);
    Log("OpenXR error: %s -> %s (%d)", what, buf, (int)r);
    return false;
}

Vec3 ToVec(const XrVector3f& v) { return { v.x, v.y, v.z }; }
Quat ToQuat(const XrQuaternionf& q) { return { q.x, q.y, q.z, q.w }; }
XrVector3f ToXr(Vec3 v) { return { v.x, v.y, v.z }; }
XrQuaternionf ToXr(Quat q) { return { q.x, q.y, q.z, q.w }; }

void WaitFence(UINT64 value)
{
    if (S.fence->GetCompletedValue() >= value) return;
    S.fence->SetEventOnCompletion(value, S.fenceEvent);
    WaitForSingleObject(S.fenceEvent, 2000);
}

bool CreateD3D12()
{
    XrGraphicsRequirementsD3D12KHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    if (!XrOk(pfnGetD3D12Reqs(S.instance, S.system, &reqs), "xrGetD3D12GraphicsRequirementsKHR")) return false;

    if (g_cfg.d3d12DebugLayer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); }
    }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { Log("CreateDXGIFactory1 failed"); return false; }
    IDXGIAdapter1* adapter = nullptr;
    HRESULT hr = factory->EnumAdapterByLuid(reqs.adapterLuid, IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(hr)) { Log("EnumAdapterByLuid failed 0x%08X", hr); return false; }

    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    Log("Headset GPU: %ls", desc.Description);

    D3D_FEATURE_LEVEL fl = reqs.minFeatureLevel > D3D_FEATURE_LEVEL_11_0 ? reqs.minFeatureLevel : D3D_FEATURE_LEVEL_11_0;
    hr = D3D12CreateDevice(adapter, fl, IID_PPV_ARGS(&S.device));
    adapter->Release();
    if (FAILED(hr)) { Log("D3D12CreateDevice failed 0x%08X", hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(S.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&S.queue)))) { Log("CreateCommandQueue failed"); return false; }
    if (FAILED(S.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&S.fence)))) { Log("CreateFence failed"); return false; }
    S.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < State::kAllocators; i++)
        if (FAILED(S.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&S.allocators[i])))) return false;
    if (FAILED(S.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.allocators[0], nullptr, IID_PPV_ARGS(&S.cmdList)))) return false;
    S.cmdList->Close();
    return true;
}

bool CreateInstance()
{
    wchar_t loaderPath[MAX_PATH];
    swprintf_s(loaderPath, L"%sopenxr_loader.dll", ModuleDir());
    if (!LoadLibraryW(loaderPath)) {
        Log("openxr_loader.dll not found next to d3d9.dll - VR disabled");
        return false;
    }

    uint32_t extCount = 0;
    if (!XrOk(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "xrEnumerateInstanceExtensionProperties")) return false;
    std::vector<XrExtensionProperties> exts(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasD3D12 = false;
    for (auto& e : exts)
        if (!strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) hasD3D12 = true;
    if (!hasD3D12) { Log("Runtime does not support XR_KHR_D3D12_enable - VR disabled"); return false; }

    const char* enabled[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "SCCT Versus VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "Unreal Engine 2.5");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled;
    if (!XrOk(xrCreateInstance(&ci, &S.instance), "xrCreateInstance")) return false;

    XrInstanceProperties props{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(S.instance, &props);
    Log("OpenXR runtime: %s %u.%u.%u", props.runtimeName,
        XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

    XrSystemGetInfo si{ XR_TYPE_SYSTEM_GET_INFO };
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk(xrGetSystem(S.instance, &si, &S.system), "xrGetSystem (is the headset connected?)")) return false;

    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(S.instance, S.system, &sp);
    Log("Headset: %s", sp.systemName);

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> vcv(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(S.instance, S.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, vcv.data());
    if (viewCount >= 1) {
        S.recommendedW = vcv[0].recommendedImageRectWidth;
        S.recommendedH = vcv[0].recommendedImageRectHeight;
        Log("Recommended per-eye resolution: %ux%u", S.recommendedW, S.recommendedH);
    }

    return XrOk(xrGetInstanceProcAddr(S.instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfnGetD3D12Reqs),
                "xrGetInstanceProcAddr(xrGetD3D12GraphicsRequirementsKHR)");
}

bool CreateSession()
{
    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = S.device;
    binding.queue = S.queue;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = S.system;
    if (!XrOk(xrCreateSession(S.instance, &sci, &S.session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!XrOk(xrCreateReferenceSpace(S.session, &rs, &S.localSpace), "xrCreateReferenceSpace(LOCAL)")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!XrOk(xrCreateReferenceSpace(S.session, &rs, &S.viewSpace), "xrCreateReferenceSpace(VIEW)")) return false;
    return true;
}

void DestroySwapchain(EyeSwapchain& sc)
{
    if (sc.handle) xrDestroySwapchain(sc.handle);
    sc.handle = XR_NULL_HANDLE;
    sc.images.clear();
}

void DestroySwapchains()
{
    DestroySwapchain(S.eyes[0]);
    DestroySwapchain(S.eyes[1]);
    DestroySwapchain(S.hud);
    DestroySwapchain(S.laser);
    if (S.laserRtv) { S.laserRtv->Release(); S.laserRtv = nullptr; }
}

bool PickFormat()
{
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(S.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(S.session, n, &n, formats.data());

    // The game writes gamma-encoded colors, so an sRGB swapchain shows them unchanged.
    struct Choice { DXGI_FORMAT dxgi; D3DFORMAT d3d9; };
    const Choice prefs[] = {
        { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3DFMT_A8R8G8B8 },
        { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3DFMT_A8B8G8R8 },
        { DXGI_FORMAT_B8G8R8A8_UNORM,      D3DFMT_A8R8G8B8 },
        { DXGI_FORMAT_R8G8B8A8_UNORM,      D3DFMT_A8B8G8R8 },
    };
    for (auto& p : prefs)
        for (int64_t f : formats)
            if (f == p.dxgi) {
                S.xrFormat = f;
                S.copyFormat = p.d3d9;
                Log("Swapchain format: DXGI %d (D3D9 copy format %d)", (int)f, (int)p.d3d9);
                return true;
            }
    Log("No usable 8-bit swapchain format offered by the runtime");
    return false;
}

bool CreateSwapchain(EyeSwapchain& sc, UINT w, UINT h)
{
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = S.xrFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!XrOk(xrCreateSwapchain(S.session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    sc.images.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)sc.images.data());
    return true;
}

bool CreateSwapchains(UINT w, UINT h)
{
    DestroySwapchains();
    if (!S.xrFormat && !PickFormat()) return false;
    if (!CreateSwapchain(S.eyes[0], w, h) || !CreateSwapchain(S.eyes[1], w, h) || !CreateSwapchain(S.hud, w, h))
        return false;
    if (CreateSwapchain(S.laser, 8, 8)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = (UINT)S.laser.images.size();
        if (FAILED(S.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&S.laserRtv)))) DestroySwapchain(S.laser);
    }
    Log("Created swapchains %ux%u", w, h);
    return true;
}

void ReleaseD3D9Resources()
{
    if (S.copyTex) { S.copyTex->Release(); S.copyTex = nullptr; }
    if (S.hudTex) { S.hudTex->Release(); S.hudTex = nullptr; }
}

// Makes sure the copy texture and swapchains match the current backbuffer.
bool EnsureResources(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    bb->Release();

    if (d.Width != S.width || d.Height != S.height || !S.eyes[0].handle) {
        if (S.frameOpen) return false; // finish the pair first (it ends with zero layers)
        ReleaseD3D9Resources();
        if (!CreateSwapchains(d.Width, d.Height)) return false;
        S.width = d.Width;
        S.height = d.Height;
        if (S.recommendedW && d.Width < S.recommendedW * 2 / 3)
            Log("Note: game resolution %ux%u is well below the headset's recommended %ux%u per eye",
                d.Width, d.Height, S.recommendedW, S.recommendedH);
    }
    if (!S.copyTex) {
        HRESULT hr = dev->CreateTexture(S.width, S.height, 1, D3DUSAGE_RENDERTARGET, S.copyFormat, D3DPOOL_DEFAULT, &S.copyTex, nullptr);
        if (FAILED(hr)) { Log("CreateTexture(copy) failed 0x%08X", hr); return false; }
    }
    if (!S.hudTex && g_cfg.hudLayer) {
        HRESULT hr = dev->CreateTexture(S.width, S.height, 1, D3DUSAGE_RENDERTARGET, S.copyFormat, D3DPOOL_DEFAULT, &S.hudTex, nullptr);
        if (FAILED(hr)) Log("CreateTexture(hud) failed 0x%08X", hr);
    }
    return true;
}

bool CopyTextureToSwapchain(IDirect3DTexture9* tex, EyeSwapchain& sc)
{
    ID3D12Resource* src = nullptr;
    HRESULT hr = S.dev9On12->UnwrapUnderlyingResource(tex, S.queue, IID_PPV_ARGS(&src));
    if (FAILED(hr)) { Log("UnwrapUnderlyingResource failed 0x%08X", hr); return false; }

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    bool acquired = XrOk(xrAcquireSwapchainImage(sc.handle, &ai, &idx), "xrAcquireSwapchainImage") &&
                    XrOk(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");

    if (acquired) {
        int a = S.nextAllocator;
        S.nextAllocator = (S.nextAllocator + 1) % State::kAllocators;
        WaitFence(S.allocatorFence[a]);
        S.allocators[a]->Reset();
        S.cmdList->Reset(S.allocators[a], nullptr);

        ID3D12Resource* target = sc.images[idx].texture;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        S.cmdList->ResourceBarrier(1, &b);

        // The unwrapped texture is in COMMON state and is implicitly promoted to COPY_SOURCE.
        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
        dl.pResource = target;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = src;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        S.cmdList->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        S.cmdList->ResourceBarrier(1, &b);
        S.cmdList->Close();
        ID3D12CommandList* lists[] = { S.cmdList };
        S.queue->ExecuteCommandLists(1, lists);
        S.allocatorFence[a] = ++S.fenceValue;
        S.queue->Signal(S.fence, S.fenceValue);
    }

    UINT64 signal = S.fenceValue;
    S.dev9On12->ReturnUnderlyingResource(tex, 1, &signal, &S.fence);
    src->Release();

    if (acquired) {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        XrOk(xrReleaseSwapchainImage(sc.handle, &ri), "xrReleaseSwapchainImage");
    }
    return acquired;
}

bool CopyBackbufferToEye(IDirect3DDevice9* dev, int eye)
{
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    S.copyTex->GetSurfaceLevel(0, &dst);
    HRESULT hr = dev->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
    bb->Release();
    dst->Release();
    if (FAILED(hr)) { Log("StretchRect failed 0x%08X", hr); return false; }
    return CopyTextureToSwapchain(S.copyTex, S.eyes[eye]);
}

// Head yaw only, so the world stays level after recentering.
Quat YawOnly(Quat q)
{
    Vec3 f = Rotate(q, { 0, 0, -1 });
    float yaw = atan2f(-f.x, -f.z);
    return { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
}

// Pose in LOCAL space -> row-vector matrix from the pose's local frame into the body camera
// space (relative to the recenter reference, left-handed, Unreal units).
D3DMATRIX PoseToBody(const XrPosef& pose)
{
    Quat refInv = Conj(S.refQ);
    Quat q = Mul(refInv, ToQuat(pose.orientation));
    Vec3 p = Rotate(refInv, ToVec(pose.position) - S.refP);
    float x = -q.x, y = -q.y, z = q.z, w = q.w;
    float R[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w) },
        { 2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
        { 2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) },
    };
    D3DMATRIX m = Identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[i][j] = R[j][i];
    m._41 = p.x * g_cfg.worldScale;
    m._42 = p.y * g_cfg.worldScale;
    m._43 = -p.z * g_cfg.worldScale;
    return m;
}

void ComputeEyeMatrices()
{
    Quat refInv = Conj(S.refQ);
    for (int e = 0; e < 2; e++) {
        Quat q = Mul(refInv, ToQuat(S.views[e].pose.orientation));
        Vec3 p = Rotate(refInv, ToVec(S.views[e].pose.position) - S.refP);

        // OpenXR (right-handed, -z forward) -> D3D view space (left-handed, +z forward)
        Quat ql = { -q.x, -q.y, q.z, q.w };
        Vec3 t = { p.x * g_cfg.worldScale, p.y * g_cfg.worldScale, -p.z * g_cfg.worldScale };

        float x = ql.x, y = ql.y, z = ql.z, w = ql.w;
        float R[3][3] = {
            { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w) },
            { 2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
            { 2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) },
        };
        // Eye pose in row-vector form is [R^T | t]; its inverse is [R | -t*R].
        D3DMATRIX H = Identity();
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                H.m[i][j] = R[i][j];
        for (int j = 0; j < 3; j++)
            H.m[3][j] = -(t.x * R[0][j] + t.y * R[1][j] + t.z * R[2][j]);
        S.eyeH[e] = H;
    }
}

void Recenter(XrTime time)
{
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (!XR_SUCCEEDED(xrLocateSpace(S.viewSpace, S.localSpace, time, &loc))) return;
    // Wait until the headset is really tracked and the session is in front, otherwise the first
    // pose can be garbage (e.g. head at the floor).
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if ((loc.locationFlags & need) != need) return;
    if (S.sessionState != XR_SESSION_STATE_FOCUSED || GetTickCount() - S.focusTick < 1000) return;
    S.refQ = YawOnly(ToQuat(loc.pose.orientation));
    S.refP = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? ToVec(loc.pose.position) : Vec3{ 0, 0, 0 };
    S.recenterPending = false;
    Log("Recentered (head at %.2f %.2f %.2f m)", S.refP.x, S.refP.y, S.refP.z);
}

// No first-person weapon drawn recently: the spy (or a merc without a weapon).
bool SpyCharacter()
{
    return GetTickCount() - S.lastWeaponTick >= 1500;
}

// What the spy's eye offsets are relative to: the top of the head, or (if the model's vertices
// cannot be read) the model's origin, which sits about at eye height but does not crouch.
Vec3 SpyEyeBase()
{
    return S.spyHeadValid ? S.spyHead : S.spyOrigin + Vec3{ 0, 0, 10.0f };
}

// Spy camera (third person) as opposed to the merc's first-person view or a zoom.
bool ThirdPerson()
{
    return g_cfg.aimMode == 2 && SpyCharacter() && !S.frameScope;
}

bool AimEnabled()
{
    if (!S.aimOn) return false;
    // Zoomed, the weapon is not drawn, but the controller (or the head) keeps aiming. SpyAim: the
    // controller also steers the spy's third-person camera (else the right stick does).
    return g_cfg.aimMode != 2 || S.frameScope || g_cfg.spyAim || !SpyCharacter();
}

// Mouse counts per degree for open-loop steering (zoom): the learned value once it is reliable.
float OpenLoopCountsPerDeg()
{
    if (g_cfg.mouseCountsPerDegree > 0) return g_cfg.mouseCountsPerDegree;
    return S.calibAccepted >= 4 ? S.countsPerDeg : 60.0f;
}

void MoveMouse(float dx, float dy)
{
    S.remX += dx;
    S.remY += dy;
    int ix = (int)S.remX, iy = (int)S.remY;
    S.remX -= ix;
    S.remY -= iy;
    input::SendMouseMove(ix, iy);
    S.lastSentX = ix;
}

// Steers the game's camera (mouse emulation) to the right controller's aim ray, and turns the
// rendered body with the thumbstick.
void AimUpdate(float dt)
{
    float yaw = S.gameYaw, pitch = S.gamePitch;
    if (!S.bodyValid) {
        S.bodyYaw = yaw;
        S.prevGameYaw = yaw;
        S.bodyValid = true;
        S.remX = S.remY = 0;
        S.lastSentX = 0;
    }

    // Large jumps were not caused by us (respawn, cutscene): keep the body relative to them.
    float observed = WrapPi(yaw - S.prevGameYaw);
    S.prevGameYaw = yaw;
    if (fabsf(observed) > 45.0f / kDeg) {
        S.bodyYaw += observed;
        observed = 0;
    }

    float kNow = g_cfg.mouseCountsPerDegree > 0 ? g_cfg.mouseCountsPerDegree : S.countsPerDeg;

    // Stall detection: during rolls, animations, death etc. the game ignores (and may buffer)
    // mouse input. Keep sending and the buffered motion is applied all at once afterwards,
    // so stop sending until the camera responds again.
    S.stallSentDeg += fabsf((float)S.lastSentX) / kNow;
    S.stallSeenDeg += fabsf(observed) * kDeg;
    S.stallTime += dt;
    if (S.stallTime > 0.15f) {
        if (S.stallSentDeg > 3.0f && S.stallSeenDeg < 0.2f * S.stallSentDeg) {
            S.aimPause = 0.35f;
            S.calibCounts = S.calibDeg = S.calibTime = 0;
        }
        S.stallSentDeg = S.stallSeenDeg = S.stallTime = 0;
    }
    if (S.aimPause > 0) S.aimPause -= dt;

    // Learn how many mouse counts turn the game camera by one degree. Once learned, only accept
    // measurements close to the known value (blocked input must not skew it).
    if (g_cfg.mouseCountsPerDegree <= 0 && S.aimPause <= 0) {
        S.calibDeg += observed * kDeg;
        S.calibTime += dt;
        if (S.calibTime > 0.3f) {
            if (fabsf(S.calibCounts) > 40 && fabsf(S.calibDeg) > 0.5f && S.calibCounts * S.calibDeg > 0) {
                float measured = S.calibCounts / S.calibDeg;
                float ratio = measured / S.countsPerDeg;
                if (S.calibAccepted < 4 || (ratio > 0.75f && ratio < 1.33f)) {
                    S.countsPerDeg += (measured - S.countsPerDeg) * (S.calibAccepted < 4 ? 0.5f : 0.15f);
                    S.countsPerDeg = fmaxf(0.05f, fminf(2000.0f, S.countsPerDeg));
                    S.calibAccepted++;
                }
            }
            S.calibCounts = S.calibDeg = S.calibTime = 0;
        }
    }
    float k = g_cfg.mouseCountsPerDegree > 0 ? g_cfg.mouseCountsPerDegree : S.countsPerDeg;

    // Turning
    if (g_cfg.snapTurn > 0) {
        if (!S.snapHeld && fabsf(S.ctl.lookX) > 0.7f) {
            S.bodyYaw += (S.ctl.lookX > 0 ? 1 : -1) * g_cfg.snapTurn / kDeg;
            S.snapHeld = true;
        } else if (fabsf(S.ctl.lookX) < 0.3f) {
            S.snapHeld = false;
        }
    } else {
        S.bodyYaw += S.ctl.lookX * g_cfg.turnSpeed / kDeg * dt;
    }
    S.bodyYaw = WrapPi(S.bodyYaw);

    // Open-loop steering while zoomed (see below): the change of a direction becomes mouse motion.
    auto steerOpenLoop = [&](float relYaw, float relPitch) {
        float aimYaw = S.bodyYaw + relYaw;
        float ko = OpenLoopCountsPerDeg() * g_cfg.scopeMouseScale;
        if (S.scopeAimValid) {
            float dYaw = WrapPi(aimYaw - S.scopeAimYaw) * kDeg;
            float dPitch = (relPitch - S.scopeAimPitch) * kDeg;
            MoveMouse(dYaw * ko, -dPitch * ko * g_cfg.mousePitchScale);
        }
        S.scopeAimYaw = aimYaw;
        S.scopeAimPitch = relPitch;
        S.scopeAimValid = true;
        S.lastSentX = 0;    // keep stall detection and calibration out of it
    };

    // Spy binoculars: look around with the head.
    if (S.frameScope && S.scopeHead) {
        if (!S.viewsValid) { S.scopeAimValid = false; return; }
        Vec3 f = Rotate(Mul(Conj(S.refQ), ToQuat(S.views[0].pose.orientation)), { 0, 0, -1 });
        steerOpenLoop(atan2f(f.x, -f.z), atan2f(f.y, sqrtf(f.x * f.x + f.z * f.z)));
        return;
    }

    if (!S.ctl.aimValid) { S.lastSentX = 0; S.scopeAimValid = false; return; }

    // Point the game camera (which sits at the recentered head position) at the spot the
    // controller ray reaches at the convergence distance.
    Quat refInv = Conj(S.refQ);
    Quat q = Mul(refInv, ToQuat(S.ctl.aim.orientation));
    Vec3 p = Rotate(refInv, ToVec(S.ctl.aim.position) - S.refP);
    // Third person: the camera is not at the head, and hand movement must not shake the view, so
    // only the controller's rotation counts.
    if (ThirdPerson()) p = { 0, 0, 0 };
    Vec3 target = p + Rotate(q, { 0, 0, -1 }) * g_cfg.aimConvergence;
    float relYaw = atan2f(target.x, -target.z);
    float relPitch = atan2f(target.y, sqrtf(target.x * target.x + target.z * target.z));

    // Zoomed, the mouse first moves the game's crosshair across the screen and the camera
    // follows it with a delay, so steering the camera in a closed loop overshoots and jumps.
    // Send the controller's own motion as plain mouse motion instead (like moving a mouse); the
    // closed loop lines the camera up with the controller again after the zoom.
    if (S.frameScope) {
        steerOpenLoop(relYaw, relPitch);
        return;
    }
    S.scopeAimValid = false;

    float errYaw = WrapPi(S.bodyYaw + relYaw - yaw) * kDeg;
    float errPitch = (relPitch - pitch) * kDeg;
    float limit = 15.0f;
    if (S.aimPause > 0) {
        S.lastSentX = 0;
        S.remX = S.remY = 0;
        return;
    }
    float cx = fmaxf(-limit, fminf(limit, errYaw * g_cfg.aimGain)) * k;
    float cy = -fmaxf(-limit, fminf(limit, errPitch * g_cfg.aimGain)) * k * g_cfg.mousePitchScale;
    MoveMouse(cx, cy);
    S.calibCounts += (float)S.lastSentX;
}

// Merc zoom: bringing the eyes down to the weapon (right controller close to the head and
// pointing where the head looks) taps the game's zoom key; lifting the head taps it again if the
// game is still zoomed.
void ScopeGesture(float dt, bool aimActive)
{
    if (!g_cfg.scopeGesture || !aimActive) return; // menus etc.: keep the state as it is
    bool on = false;
    if (S.ctl.aimValid && S.viewsValid) {
        Vec3 head = (ToVec(S.views[0].pose.position) + ToVec(S.views[1].pose.position)) * 0.5f;
        Vec3 headFwd = Rotate(ToQuat(S.views[0].pose.orientation), { 0, 0, -1 });
        Vec3 hand = ToVec(S.ctl.aim.position);
        Vec3 aimFwd = Rotate(ToQuat(S.ctl.aim.orientation), { 0, 0, -1 });
        float limit = g_cfg.scopeGestureDistance * (S.scopeGestureOn ? 1.35f : 1.0f);
        float minAlign = S.scopeGestureOn ? 0.7f : 0.82f;   // ~45 / ~35 degrees
        on = Length(hand - head) < limit && Dot(headFwd, aimFwd) > minAlign && Dot(hand - head, headFwd) > -0.05f;
    }
    if (on == S.scopeGestureOn) { S.scopeDwell = 0; }
    else if ((S.scopeDwell += dt) > 0.15f) {
        S.scopeDwell = 0;
        S.scopeGestureOn = on;
        if (on) {
            S.scopeSeen = S.gameZoomed;
            S.scopeTapped = !S.gameZoomed;
            if (S.scopeTapped) input::TapScopeKey();
            Log("Scope gesture on%s", S.scopeTapped ? " - zoom key sent" : " (game already zoomed)");
        } else {
            // Still zoomed, or the key did something that is not a zoom: tap again to undo it.
            // If the game left the zoom by itself (reload, damage), do nothing.
            bool tap = S.gameZoomed || (S.scopeTapped && !S.scopeSeen);
            if (tap) input::TapScopeKey();
            Log("Scope gesture off%s", tap ? " - zoom key sent" : "");
        }
    }
    if (S.scopeGestureOn && S.gameZoomed) S.scopeSeen = true;
}

// Merc reload: the left hand brought to the magazine of the gun in the right hand taps the
// reload key, like changing the magazine by hand. Once per approach: the hand has to leave the
// magazine again before the next one.
void ReloadGesture(float dt, bool mercAiming)
{
    if (!g_cfg.reloadGesture) return;
    bool in = false;
    float dist = 0;
    if (mercAiming && S.ctl.aimValid && S.ctl.leftValid) {
        Quat q = ToQuat(S.ctl.aim.orientation);
        Vec3 mag = ToVec(S.ctl.aim.position) + Rotate(q, { 0, -g_cfg.reloadDown, -g_cfg.reloadForward });
        dist = Length(ToVec(S.ctl.left.position) - mag);
        in = dist < g_cfg.reloadRadius * (S.reloadIn ? 1.4f : 1.0f);
    }
    if (!in) {
        S.reloadIn = false;
        S.reloadDwell = 0;
        return;
    }
    if (S.reloadIn) return;
    if ((S.reloadDwell += dt) < 0.08f) return;
    S.reloadIn = true;
    if (GetTickCount() - S.reloadTick < 1000) return;   // not twice in a row
    S.reloadTick = GetTickCount();
    input::TapReloadKey();
    Log("Reload gesture: left hand %.0f cm from the magazine - reload key sent", dist * 100);
}

// Intersection of the right controller ray with the virtual menu screen.
bool MenuRayHit(float* t, float* u, float* v)
{
    if (!S.ctl.aimValid || !S.width) return false;
    Vec3 center = S.refP + Rotate(S.refQ, { 0, 0, -g_cfg.menuQuadDistance });
    Vec3 normal = Rotate(S.refQ, { 0, 0, 1 });
    Vec3 o = ToVec(S.ctl.aim.position);
    Vec3 dir = Rotate(ToQuat(S.ctl.aim.orientation), { 0, 0, -1 });
    float denom = Dot(dir, normal);
    if (denom > -1e-4f) return false; // pointing away from the screen
    *t = Dot(center - o, normal) / denom;
    if (*t <= 0) return false;
    Vec3 hit = o + dir * *t - center;
    float w = g_cfg.menuQuadWidth, h = w * S.height / (float)S.width;
    *u = Dot(hit, Rotate(S.refQ, { 1, 0, 0 })) / w + 0.5f;
    *v = 0.5f - Dot(hit, Rotate(S.refQ, { 0, 1, 0 })) / h;
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

// Menus: the game's cursor only reacts to relative mouse motion, so steer it (closed loop, like
// weapon aiming) towards the point where the controller ray hits the virtual screen.
void MenuLaserCursor(float dt)
{
    float t, u, v;
    if (!g_cfg.menuLaser || !S.cursorValid || !MenuRayHit(&t, &u, &v)) return;
    if (fabsf(S.ctl.lookX) > 0 || fabsf(S.ctl.lookY) > 0) return; // the stick has priority

    // Learn mouse counts per cursor pixel from how far the cursor moved.
    if (S.menuPrevValid) {
        S.menuCalibCounts += fabsf((float)S.menuSentX) + fabsf((float)S.menuSentY);
        S.menuCalibPx += fabsf(S.cursorX - S.menuPrevX) + fabsf(S.cursorY - S.menuPrevY);
        S.menuCalibTime += dt;
        if (S.menuCalibTime > 0.3f) {
            if (S.menuCalibCounts > 30 && S.menuCalibPx > 3) {
                float measured = S.menuCalibCounts / S.menuCalibPx;
                S.menuCountsPerPx += (measured - S.menuCountsPerPx) * 0.5f;
                S.menuCountsPerPx = fmaxf(0.05f, fminf(200.0f, S.menuCountsPerPx));
            }
            S.menuCalibCounts = S.menuCalibPx = S.menuCalibTime = 0;
        }
    }
    S.menuPrevX = S.cursorX;
    S.menuPrevY = S.cursorY;
    S.menuPrevValid = true;

    float ex = u * S.cursorSpaceW - S.cursorX;
    float ey = v * S.cursorSpaceH - S.cursorY;
    if (fabsf(ex) < 0.5f && fabsf(ey) < 0.5f) { S.menuSentX = S.menuSentY = 0; return; }
    float dx = ex * S.menuCountsPerPx * 0.6f, dy = ey * S.menuCountsPerPx * 0.6f;
    S.remX += dx;
    S.remY += dy;
    int ix = (int)S.remX, iy = (int)S.remY;
    S.remX -= ix;
    S.remY -= iy;
    input::SendMouseMove(ix, iy);
    S.menuSentX = ix;
    S.menuSentY = iy;
}

Quat FromBasis(Vec3 X, Vec3 Y, Vec3 Z)
{
    float m00 = X.x, m01 = Y.x, m02 = Z.x;
    float m10 = X.y, m11 = Y.y, m12 = Z.y;
    float m20 = X.z, m21 = Y.z, m22 = Z.z;
    float tr = m00 + m11 + m22, s;
    if (tr > 0) {
        s = sqrtf(tr + 1.0f) * 2;
        return { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s };
    }
    if (m00 > m11 && m00 > m22) {
        s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        return { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
    }
    if (m11 > m22) {
        s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        return { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
    }
    s = sqrtf(1.0f + m22 - m00 - m11) * 2;
    return { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
}

// Fills the laser swapchain with a solid colour.
bool FillLaserImage()
{
    if (!S.laser.handle || !S.laserRtv) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(S.laser.handle, &ai, &idx))) return false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(S.laser.handle, &wi))) {
        int a = S.nextAllocator;
        S.nextAllocator = (S.nextAllocator + 1) % State::kAllocators;
        WaitFence(S.allocatorFence[a]);
        S.allocators[a]->Reset();
        S.cmdList->Reset(S.allocators[a], nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = S.laserRtv->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += idx * S.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = (DXGI_FORMAT)S.xrFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        S.device->CreateRenderTargetView(S.laser.images[idx].texture, &rd, rtv);
        const float red[4] = { 1.0f, 0.12f, 0.08f, 1.0f };
        S.cmdList->ClearRenderTargetView(rtv, red, 0, nullptr);
        S.cmdList->Close();
        ID3D12CommandList* lists[] = { S.cmdList };
        S.queue->ExecuteCommandLists(1, lists);
        S.allocatorFence[a] = ++S.fenceValue;
        S.queue->Signal(S.fence, S.fenceValue);
    }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(S.laser.handle, &ri);
    return true;
}

// Menu laser: a beam from the right controller to the virtual screen and a dot where it hits.
uint32_t MenuLaserLayers(XrCompositionLayerQuad* beam, XrCompositionLayerQuad* dot)
{
    if (!g_cfg.menuLaser || !S.ctl.aimValid || !FillLaserImage()) return 0;
    Vec3 o = ToVec(S.ctl.aim.position);
    Vec3 dir = Rotate(ToQuat(S.ctl.aim.orientation), { 0, 0, -1 });
    float t, u, v;
    bool hit = MenuRayHit(&t, &u, &v);
    float len = hit ? t : 3.0f;
    Vec3 center = o + dir * (len * 0.5f);
    Vec3 head = (ToVec(S.views[0].pose.position) + ToVec(S.views[1].pose.position)) * 0.5f;
    Vec3 toHead = head - center;
    Vec3 Z = toHead - dir * Dot(toHead, dir);
    if (Length(Z) < 1e-4f) return 0;
    Z = Normalize(Z);
    Vec3 X = Cross(dir, Z);

    for (XrCompositionLayerQuad* q : { beam, dot }) {
        *q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        q->space = S.localSpace;
        q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q->subImage.swapchain = S.laser.handle;
        q->subImage.imageRect = { { 0, 0 }, { 8, 8 } };
    }
    beam->pose.orientation = ToXr(FromBasis(X, dir, Z));
    beam->pose.position = ToXr(center);
    beam->size = { 0.004f, len };
    if (!hit) return 1;
    dot->pose.orientation = ToXr(S.refQ);
    dot->pose.position = ToXr(o + dir * (t - 0.01f));
    dot->size = { 0.025f, 0.025f };
    return 2;
}

void UpdateControls(bool sceneLast)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = S.lastPairTime.QuadPart ? (float)(now.QuadPart - S.lastPairTime.QuadPart) / freq.QuadPart : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    S.lastPairTime = now;

    bool focused = S.sessionState == XR_SESSION_STATE_FOCUSED && input::GameHasFocus();
    // Menu controls (arrows, Enter, Esc) in menus, while the in-game menu shows its cursor, and
    // after the in-game menu was opened with the controller's Menu button.
    // (Zoomed, small pretransformed sprites of the zoom overlay are not a menu cursor.)
    bool cursorSeen = !S.frameScope && S.menuUiTick && GetTickCount() - S.menuUiTick < 500;
    bool inGameMenu = S.inGameMenuTick && GetTickCount() - S.inGameMenuTick < 250;
    bool menuUi = !sceneLast || cursorSeen || inGameMenu;
    bool aimActive = sceneLast && !menuUi && AimEnabled() && g_cfg.worldUpAxis == 2;
    // Spy controls also while the spy aims a weapon or looks through the binoculars (zoomed, the
    // merc's weapon is not drawn either, so the zoom remembers who started it).
    bool spyChar = g_cfg.aimMode == 2 && (S.frameScope ? S.scopeHead : SpyCharacter());
    bool spyMode = sceneLast && !menuUi && (!aimActive || spyChar);
    if (S.xrapi) {
        if (S.viewsValid)
            input::UpdateXrApi(S.xrPacket, focused, aimActive, menuUi, spyMode, g_cfg.xrApiControllerRoll, &S.ctl);
        else
            S.ctl = input::Controls();
    } else {
        input::Update(S.session, S.localSpace, S.frameState.predictedDisplayTime, focused, aimActive, menuUi, spyMode, &S.ctl);
    }

    if (menuUi != S.lastMenuUi) {
        Log("Controller mode: %s%s%s", menuUi ? "menu" : "game", cursorSeen ? " (menu cursor seen)" : "",
            inGameMenu ? " (in-game menu)" : "");
        S.lastMenuUi = menuUi;
    }
    if (S.ctl.recenter) S.recenterPending = true;
    if (S.ctl.fpvToggle) {
        if (!S.spyFpv && !(g_cfg.aimMode == 2 && SpyCharacter())) {
            Log("First-person view: only for the spy");
        } else {
            S.spyFpv = !S.spyFpv;
            Log("Spy first-person view %s%s", S.spyFpv ? "ON" : "off",
                S.spyFpv && !S.spyOriginValid ? " (spy model not found yet)" : "");
        }
    }

    bool keyDown = g_cfg.aimToggleKey && (GetAsyncKeyState(g_cfg.aimToggleKey) & 0x8000);
    if ((keyDown && !S.aimKeyDown) || S.ctl.toggleAim) {
        S.aimOn = !S.aimOn;
        S.bodyValid = false;
        Log("Aim mode %s", S.aimOn ? "ON (controller aims)" : "OFF (mouse/stick aims)");
    }
    S.aimKeyDown = keyDown;

    static bool loggedFocus = false;
    if (!focused) {
        if (!loggedFocus && S.sessionState == XR_SESSION_STATE_FOCUSED) {
            Log("Game window is not in the foreground - controller input paused (click the game window)");
            loggedFocus = true;
        }
        return;
    }
    loggedFocus = false;

    ScopeGesture(dt, aimActive || spyMode);
    ReloadGesture(dt, aimActive && !spyMode && !S.frameScope);
    // Camera motion seen while zoomed was not caused by counted mouse output: do not learn from it.
    if (S.frameScope != S.scopeAimLast) {
        if (S.frameScope) {
            // The merc's weapon was on screen until the zoom started; the spy has none.
            S.scopeHead = g_cfg.aimMode == 2 && SpyCharacter();
            S.zoomStartTick = GetTickCount();
            S.eyeCalibDone = false;
            Log("Zoom steered by the %s", S.scopeHead ? "head (spy binoculars)" : "controller (merc)");
        }
        S.calibCounts = S.calibDeg = S.calibTime = 0;
        S.stallSentDeg = S.stallSeenDeg = S.stallTime = 0;
        S.scopeAimValid = false;
        S.scopeAimLast = S.frameScope;
    }

    if (aimActive) {
        AimUpdate(dt);
    } else {
        // Menus, or aim mode off: the right stick moves the mouse.
        S.bodyValid = false;
        float speed = menuUi ? g_cfg.menuMouseSpeed : g_cfg.stickMouseSpeed;
        MoveMouse(S.ctl.lookX * speed * dt, -S.ctl.lookY * speed * dt);
        if (menuUi) MenuLaserCursor(dt);
    }
}

void BeginPair()
{
    // (A zoomed frame ends its XR frame on its own, without a second eye.)
    bool sceneLast = ((S.eyeScene[0] && S.eyeScene[1]) || S.eyeScope[0]) && S.gameAnglesValid;

    XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
    S.frameState = { XR_TYPE_FRAME_STATE };
    if (!XrOk(xrWaitFrame(S.session, &wi, &S.frameState), "xrWaitFrame")) return;
    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!XrOk(xrBeginFrame(S.session, &bi), "xrBeginFrame")) return;
    S.frameOpen = true;
    S.curEye = 0;
    S.eyeCopied[0] = S.eyeCopied[1] = false;
    S.eyeScene[0] = S.eyeScene[1] = false;
    S.eyeScope[0] = S.eyeScope[1] = false;
    S.hudCopied = false;
    S.hudDrawnPair = false;
    S.sceneLastPair = sceneLast;

    if (S.recenterPending) Recenter(S.frameState.predictedDisplayTime);

    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = S.frameState.predictedDisplayTime;
    li.space = S.localSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    S.views[0] = { XR_TYPE_VIEW };
    S.views[1] = { XR_TYPE_VIEW };
    S.viewsValid = XR_SUCCEEDED(xrLocateViews(S.session, &li, &vs, 2, &n, S.views)) && n == 2 &&
                   (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    static bool loggedFov = false;
    if (S.viewsValid && !loggedFov) {
        loggedFov = true;
        for (int e = 0; e < 2; e++)
            Log("Eye %d FOV: left %.1f right %.1f up %.1f down %.1f deg", e,
                S.views[e].fov.angleLeft * kDeg, S.views[e].fov.angleRight * kDeg,
                S.views[e].fov.angleUp * kDeg, S.views[e].fov.angleDown * kDeg);
    }

    UpdateControls(sceneLast);
    S.gameAnglesValid = false;
    if (S.viewsValid) ComputeEyeMatrices();
    S.handValid = S.ctl.aimValid;
    if (S.handValid) S.handMat = PoseToBody(S.ctl.aim);
}

void EndPair()
{
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad hudQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad beam, dot;
    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layerCount = 0;

    bool haveImages = S.frameState.shouldRender && S.viewsValid && S.eyeCopied[0] && S.eyeCopied[1];
    int last = S.curEye;
    bool scope = S.frameState.shouldRender && S.viewsValid && S.eyeScope[last] && S.eyeCopied[last];
    bool scene = haveImages && S.eyeScene[0] && S.eyeScene[1] && !scope;
    bool hud = S.frameState.shouldRender && S.hudCopied && !scope;
    if (scope) {
        // Zoomed: the game's own flat image (zoom overlay and HUD included) on a screen fixed in
        // front of the eyes, sized like the game's normal field of view so the zoom looks right.
        int e = last;
        float d = g_cfg.scopeDistance;
        float w = 2 * d * S.normalTanX * g_cfg.scopeSize;
        quad.space = S.viewSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = S.eyes[e].handle;
        quad.subImage.imageRect = { { 0, 0 }, { (int32_t)S.width, (int32_t)S.height } };
        quad.pose.orientation = { 0, 0, 0, 1 };
        quad.pose.position = { 0, 0, -d };
        quad.size = { w, w * S.height / (float)S.width };
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
        S.scopePairs++;
    } else if (scene) {
        for (int e = 0; e < 2; e++) {
            pv[e].pose = S.views[e].pose;
            pv[e].fov = S.views[e].fov;
            pv[e].subImage.swapchain = S.eyes[e].handle;
            pv[e].subImage.imageRect = { { 0, 0 }, { (int32_t)S.width, (int32_t)S.height } };
        }
        proj.space = S.localSpace;
        proj.viewCount = 2;
        proj.views = pv;
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
        S.stereoViews++;
    } else if (S.frameState.shouldRender && S.eyeCopied[0]) {
        // Menus / loading screens: show the flat image on a virtual screen in front of the player.
        quad.space = S.localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = S.eyes[0].handle;
        quad.subImage.imageRect = { { 0, 0 }, { (int32_t)S.width, (int32_t)S.height } };
        quad.pose.orientation = ToXr(S.refQ);
        quad.pose.position = ToXr(S.refP + Rotate(S.refQ, { 0, 0, -g_cfg.menuQuadDistance }));
        quad.size = { g_cfg.menuQuadWidth, g_cfg.menuQuadWidth * S.height / (float)S.width };
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
        S.quadPairs++;
    }

    if (hud) {
        hudQuad.space = S.localSpace;
        hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied
        hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        hudQuad.subImage.swapchain = S.hud.handle;
        hudQuad.subImage.imageRect = { { 0, 0 }, { (int32_t)S.width, (int32_t)S.height } };
        if (scene && S.gameAnglesValid) {
            // Cover exactly the game camera's frustum so projected markers line up with the world.
            // In aim mode the game camera follows the controller; by default keep the HUD in front of the body.
            bool aiming = S.aimOn && S.bodyValid;
            bool follow = !aiming || S.hudFollow;
            Quat q = (follow && S.hudRelValid) ? Mul(S.refQ, S.hudRel) : S.refQ;
            float scale = (aiming && S.hudFollow) ? S.hudFollowScale : g_cfg.hudScale;
            float d = S.hudDistance;
            hudQuad.pose.orientation = ToXr(q);
            hudQuad.pose.position = ToXr(S.refP + Rotate(q, { 0, 0, -d }));
            hudQuad.size = { 2 * d * S.gameTanX * scale, 2 * d * S.gameTanY * scale };
        } else {
            hudQuad.pose.orientation = ToXr(S.refQ);
            hudQuad.pose.position = ToXr(S.refP + Rotate(S.refQ, { 0, 0, -g_cfg.menuQuadDistance }));
            hudQuad.size = { g_cfg.menuQuadWidth, g_cfg.menuQuadWidth * S.height / (float)S.width };
        }
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&hudQuad;
        S.hudPairs++;
    }

    if (!scene && !scope && S.frameState.shouldRender && S.eyeCopied[0]) {
        uint32_t n = MenuLaserLayers(&beam, &dot);
        if (n >= 1) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&beam;
        if (n >= 2) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&dot;
    }

    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = S.frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrOk(xrEndFrame(S.session, &ei), "xrEndFrame");
    S.frameOpen = false;
    S.pairs++;
}

void PollEvents()
{
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(S.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            S.sessionState = e->state;
            Log("Session state -> %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                S.running = XrOk(xrBeginSession(S.session, &bi), "xrBeginSession");
                S.recenterPending = true;
            } else if (e->state == XR_SESSION_STATE_FOCUSED) {
                S.focusTick = GetTickCount();
                S.recenterPending = true;
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                if (S.frameOpen) EndPair();
                xrEndSession(S.session);
                S.running = false;
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                S.running = false;
                S.ok = false;
                Log("XR session lost - continuing in flat mode");
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            S.recenterPending = true;
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            S.running = false;
            S.ok = false;
            Log("XR instance lost - continuing in flat mode");
            break;
        default:
            break;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

void PrintStats()
{
    DWORD now = GetTickCount();
    if (!S.statsStart) S.statsStart = now;
    DWORD elapsed = now - S.statsStart;
    if (g_cfg.statsIntervalSec <= 0 || elapsed < (DWORD)g_cfg.statsIntervalSec * 1000) return;
    float sec = elapsed / 1000.0f;
    Log("Stats: game %.1f fps, XR %.1f pairs/s (stereo %u, flat-screen %u, zoom %u, hud %u), running=%d, aim %d, body %d, mouse %.2f counts/deg, focus %d%s",
        S.presents / sec, S.pairs / sec, S.stereoViews, S.quadPairs, S.scopePairs, S.hudPairs, S.running, S.aimOn, S.bodyValid,
        g_cfg.mouseCountsPerDegree > 0 ? g_cfg.mouseCountsPerDegree : S.countsPerDeg, input::GameHasFocus(),
        S.spyFpv ? ", spy first person" : "");
    if (S.spyFpv) Log("Spy first person: %.1f hidden draws/frame (own model)", S.hiddenDraws / (float)(S.presents ? S.presents : 1));
    if (S.skipNoResources || S.skipNoRender || S.skipNoViews || S.skipCopyFailed)
        Log("Frames not sent to the headset: %u no resources, %u runtime says do not render, %u no head tracking, %u copy failed",
            S.skipNoResources, S.skipNoRender, S.skipNoViews, S.skipCopyFailed);
    S.skipNoResources = S.skipNoRender = S.skipNoViews = S.skipCopyFailed = 0;
    if (S.xrapi) {
        static unsigned lastPackets;
        Log("XrAPI: %.1f tracking packets/s, sync %d, tracking %s", (S.xrPackets - lastPackets) / sec, S.xrSync,
            S.viewsValid ? "ok" : "LOST");
        lastPackets = S.xrPackets;
        char raw[256];
        rawinject::Stats(raw, sizeof(raw));
        Log("%s", raw);
        dihook::Stats(raw, sizeof(raw));
        Log("%s", raw);
    }
    S.presents = S.pairs = S.stereoViews = S.quadPairs = S.hudPairs = S.scopePairs = S.hiddenDraws = 0;
    S.statsStart = now;
}

// --- WinlatorXR XR API backend -------------------------------------------------------------

// Start of a game frame: the latest tracking packet gives both eye poses (head pose +/- half
// the IPD) and the FOV; this frame renders the next eye in turn.
void XrApiBeginFrame()
{
    // The HUD texture the device hooks draw the 2D layer into (recreated after a device reset).
    if (!S.hudTex && S.dev9 && g_cfg.hudLayer && S.width) {
        HRESULT hr = S.dev9->CreateTexture(S.width, S.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                           D3DPOOL_DEFAULT, &S.hudTex, nullptr);
        if (FAILED(hr)) Log("CreateTexture(hud) failed 0x%08X", hr);
    }

    xrapi::Packet p;
    DWORD age = 0;
    S.viewsValid = xrapi::Latest(&p, &age, &S.xrPackets) && age < 1000;
    S.curEye = S.xrEyeNext;
    S.xrEyeNext ^= 1;
    if (S.viewsValid) {
        Quat hq = { p.headQ[0], p.headQ[1], p.headQ[2], p.headQ[3] };
        Vec3 hp = { p.headPos[0], p.headPos[1], p.headPos[2] };
        if (S.recenterPending) {
            S.refQ = YawOnly(hq);
            S.refP = hp;
            S.recenterPending = false;
            Log("Recentered (head at %.2f %.2f %.2f m)", hp.x, hp.y, hp.z);
        }
        float half = (p.ipd > 0.04f && p.ipd < 0.09f ? p.ipd : 0.063f) * 0.5f;
        if (!g_cfg.xrApiStereo) half = 0.0f;    // mono: both eyes see the head-center view
        float fx = (p.fovX > 1.0f ? p.fovX : 100.0f) * 0.5f / kDeg;
        float fy = (p.fovY > 1.0f ? p.fovY : 100.0f) * 0.5f / kDeg;
        for (int e = 0; e < 2; e++) {
            S.views[e].pose.orientation = ToXr(hq);
            S.views[e].pose.position = ToXr(hp + Rotate(hq, { e ? half : -half, 0, 0 }));
            S.views[e].fov = { -fx, fx, fy, -fy };
        }
        S.xrSync = p.sync;
        S.xrPacket = p;
        if (!S.xrLoggedFov) {
            S.xrLoggedFov = true;
            Log("XrAPI: eye FOV %.1f x %.1f deg, IPD %.1f mm", fx * 2 * kDeg, fy * 2 * kDeg, half * 2000);
        }
    }

    bool sceneLast = S.xrSceneFrames >= 2 && S.gameAnglesValid;
    S.sceneLastPair = sceneLast;
    UpdateControls(sceneLast);
    S.gameAnglesValid = false;
    if (S.viewsValid) ComputeEyeMatrices();
    S.handValid = S.ctl.aimValid;
    if (S.handValid) S.handMat = PoseToBody(S.ctl.aim);
    S.frameOpen = true;
}

// End of a game frame (right before Present): tell WinlatorXR how to show it and stamp the
// frame with its pose's sync value (red) and its eye (blue).
void XrApiEndFrame(IDirect3DDevice9* dev)
{
    if (!S.frameOpen) return;
    S.frameOpen = false;
    // Zoomed frames keep the game's camera (no stereo draws) but are still gameplay.
    bool zoomed = S.frameScope && S.anglesThisFrame;
    bool scene = S.frameSceneDraws >= (unsigned)g_cfg.minSceneDraws || zoomed;
    S.xrSceneFrames = scene ? S.xrSceneFrames + 1 : 0;
    // Menus, loading screens and the zoom (binoculars / sniper mode): a flat screen.
    bool immersive = S.xrSceneFrames >= 2 && !zoomed;
    if (scene) S.stereoViews++;
    else S.quadPairs++;

    int vrMode = immersive ? xrapi::kVrImmersive : xrapi::kVrScreen;
    // Stereo: one eye per game frame (alternate eyes). Mono: every frame shown to both eyes.
    bool stereo = immersive && g_cfg.xrApiStereo;
    int mode3D = stereo ? xrapi::k3DAlternateEye : xrapi::k3DFlat;
    DWORD now = GetTickCount();
    if (vrMode != S.xrSentVr || mode3D != S.xrSent3D || now - S.xrStateTick > 1000) {
        if (vrMode != S.xrSentVr)
            Log("XrAPI: %s", immersive ? (stereo ? "immersive VR, alternate eyes" : "immersive VR, mono")
                                       : zoomed ? "screen mode (zoom)" : "screen mode (menu)");
        xrapi::SendState(vrMode, mode3D, 0.0f, 0.0f);
        S.xrSentVr = vrMode;
        S.xrSent3D = mode3D;
        S.xrStateTick = now;
    }

    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        RECT r = { 0, 0, 10, 10 };
        dev->ColorFill(bb, &r, D3DCOLOR_XRGB(S.xrSync & 0xFF, 0, stereo && S.curEye ? 255 : 0));
        bb->Release();
    }
    S.pairs++;
}

} // namespace

bool UsesXrApi() { return S.xrapi; }

bool Init()
{
    if (S.initTried) return S.ok;
    S.initTried = true;

    // Standalone headset in a WinlatorXR container: its XR API replaces OpenXR.
    if (g_cfg.backend == 2 || (g_cfg.backend == 0 && xrapi::Available())) {
        S.ok = S.xrapi = xrapi::Start();
        Log(S.ok ? "VR initialized (WinlatorXR XR API)" : "WinlatorXR XR API not available - running in flat mode");
        if (S.ok) {
            S.running = true;
            S.sessionState = XR_SESSION_STATE_FOCUSED;
            S.frameState.shouldRender = XR_TRUE;
            // The controllers come in the tracking packet and go through the same bindings as
            // with OpenXR. The zoom is shown with WinlatorXR's screen mode.
            input::InitXrApi();
            S.aimOn = g_cfg.aimMode != 0;
            S.hudFollow = g_cfg.hudFollowAim;
            S.hudDistance = g_cfg.hudDistance;
            S.hudFollowScale = g_cfg.hudFollowScale;
            S.eyeRight = g_cfg.spyEyeRight;
            S.eyeUp = g_cfg.spyEyeUp;
            S.eyeFwd = g_cfg.spyEyeForward;
        }
        return S.ok;
    }

    S.ok = CreateInstance() && CreateD3D12() && CreateSession();
    Log(S.ok ? "VR initialized" : "VR initialization failed - running in flat mode");
    if (S.ok) {
        input::Init(S.instance, S.session);
        S.aimOn = g_cfg.aimMode != 0;
        S.hudFollow = g_cfg.hudFollowAim;
        S.hudDistance = g_cfg.hudDistance;
        S.hudFollowScale = g_cfg.hudFollowScale;
        S.eyeRight = g_cfg.spyEyeRight;
        S.eyeUp = g_cfg.spyEyeUp;
        S.eyeFwd = g_cfg.spyEyeForward;
    }
    return S.ok;
}

bool Available() { return S.ok; }
IUnknown* D3D12Device() { return S.device; }
IUnknown* D3D12Queue() { return S.queue; }

void OnDeviceCreated(IDirect3DDevice9* dev, HWND wnd)
{
    if (!S.ok) return;
    S.gameWnd = wnd;
    input::SetGameWindow(wnd);
    if (S.xrapi) {
        S.dev9 = dev;
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            S.width = d.Width;
            S.height = d.Height;
            bb->Release();
        }
        Log("Device ready (XR API, %ux%u)", S.width, S.height);
        rawinject::Install(wnd);
        input::InstallKeyStateHooks();
        return;
    }
    if (S.dev9On12) { S.dev9On12->Release(); S.dev9On12 = nullptr; }
    ReleaseD3D9Resources();
    S.dev9 = dev;
    if (FAILED(dev->QueryInterface(__uuidof(IDirect3DDevice9On12), (void**)&S.dev9On12))) {
        Log("Device is not a D3D9On12 device - VR disabled");
        S.ok = false;
        return;
    }
    Log("D3D9On12 device ready");
}

void OnPreReset()
{
    ReleaseD3D9Resources();
}

void OnPostReset(IDirect3DDevice9* dev)
{
    // XR API: the game switches to its fullscreen size with a Reset after startup; the HUD
    // texture (recreated next frame) and its projection have to use the new backbuffer size.
    if (S.xrapi && dev == S.dev9) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            S.width = d.Width;
            S.height = d.Height;
            bb->Release();
        }
        Log("Backbuffer after Reset: %ux%u", S.width, S.height);
    }
}

void OnPresent(IDirect3DDevice9* dev)
{
    if (!S.ok || dev != S.dev9) return;
    S.presents++;
    PrintStats();

    if (g_cfg.hudToggleKey) {
        bool down = (GetAsyncKeyState(g_cfg.hudToggleKey) & 0x8000) != 0;
        if (down && !S.hudKeyDown) {
            S.hudFollow = !S.hudFollow;
            Log("HUD mode: %s", S.hudFollow ? "follows the weapon (markers line up)" : "fixed in front of the body");
        }
        S.hudKeyDown = down;
    }

    // Num+ / Num-: make the follow-the-weapon HUD panel bigger / smaller, so objective markers
    // can be matched to their targets.
    {
        bool biggerKey = (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
        bool smallerKey = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
        if ((biggerKey && !S.hudFarKey) || (smallerKey && !S.hudNearKey)) {
            S.hudFollowScale += biggerKey ? 0.05f : -0.05f;
            if (S.hudFollowScale < 0.2f) S.hudFollowScale = 0.2f;
            if (S.hudFollowScale > 1.5f) S.hudFollowScale = 1.5f;
            Log("HUD size (follow mode): %.2f (put HudFollowScale=%.2f in SCCT_VR.ini to keep it)",
                S.hudFollowScale, S.hudFollowScale);
        }
        S.hudFarKey = biggerKey;
        S.hudNearKey = smallerKey;
    }

    if (g_cfg.recenterKey) {
        bool down = (GetAsyncKeyState(g_cfg.recenterKey) & 0x8000) != 0;
        if (down && !S.recenterKeyDown) S.recenterPending = true;
        S.recenterKeyDown = down;
    }

    bool resources = !S.xrapi && S.running && EnsureResources(dev);

    if (S.xrapi) {
        XrApiEndFrame(dev);
    } else if (S.frameOpen) {
        int eye = S.curEye;
        // Zoomed frames are drawn without the eye camera, so they have no stereo draws.
        S.eyeScope[eye] = S.frameScope && S.anglesThisFrame;
        S.eyeScene[eye] = S.frameSceneDraws >= (unsigned)g_cfg.minSceneDraws || S.eyeScope[eye];
        if (resources && S.frameState.shouldRender && S.viewsValid)
            S.eyeCopied[eye] = CopyBackbufferToEye(dev, eye);
        // Why frames do not reach the headset (statistics)
        if (!resources) S.skipNoResources++;
        else if (!S.frameState.shouldRender) S.skipNoRender++;
        else if (!S.viewsValid) S.skipNoViews++;
        else if (!S.eyeCopied[eye]) S.skipCopyFailed++;
        S.hudDrawnPair = S.hudDrawnPair || S.hudDrawn;
        // One acquire/release of the HUD swapchain per XR frame: some runtimes show every released
        // image immediately, so a second copy per pair made the HUD alternate between frames.
        if (eye == 1 && resources && S.hudDrawnPair && S.hudTex && S.frameState.shouldRender)
            S.hudCopied = CopyTextureToSwapchain(S.hudTex, S.hud);
        // Zoomed frames need no second eye: every game frame is its own XR frame, so the zoom
        // image and the aiming (mouse steering runs per XR frame) update at the full game rate.
        if (eye == 1 || S.eyeScope[eye]) EndPair();
        else S.curEye = 1;
    }

    if (!S.xrapi) PollEvents();
    if (!S.anglesThisFrame) S.frameScope = S.gameZoomed = false; // no game camera (menu, loading)
    S.hudDrawn = false;
    S.anglesThisFrame = false;
    S.hudRelThisFrame = false;
    S.frameSceneDraws = 0;

    // Main camera of this frame = the position most 3D draws used.
    int best = -1;
    for (int i = 0; i < S.camSlotCount; i++)
        if (best < 0 || S.camSlotDraws[i] > S.camSlotDraws[best]) best = i;
    S.mainCamValid = best >= 0 && S.camSlotDraws[best] > 0;
    if (S.mainCamValid) S.mainCamPos = S.camSlotPos[best];
    S.camSlotCount = 0;
    S.curCamSlot = -1;
    if (S.spyCamFrame) {
        // Spy position for the next frame (kept as is while zoomed / not seen).
        bool found = S.spyBestPrims > 0;
        if (found != S.spyOriginValid)
            Log(found ? "Spy model found (%u triangles) - VR view placed relative to the spy" : "Spy model lost", S.spyBestPrims);
        S.spyOriginValid = found;
        if (found) S.spyOrigin = S.spyBestOrigin;
        S.spyHeadValid = found && S.spyBestHeadValid;
        if (S.spyHeadValid) S.spyHead = S.spyBestHead;
        if (found && !S.loggedHead) {
            S.loggedHead = true;
            if (S.spyHeadValid)
                Log("Spy head found: top %.0f UU above the model origin", S.spyHead.z - S.spyOrigin.z);
            else
                Log("Spy head not readable from the model - first person uses the model origin");
        }
    }
    S.spyBestPrims = 0;
    S.spyCamFrame = false;

    if (S.xrapi)
        XrApiBeginFrame();
    else if (S.running && !S.frameOpen && S.ok && S.width)
        BeginPair();
}

// The HUD panel (same pose as the OpenXR HUD layer: follows the game camera so markers sit on
// their targets) projected into eye `e`'s image: 4 pretransformed vertices in backbuffer pixels.
bool HudPanelVerts(int e, float verts[4][6])
{
    if (!S.viewsValid || !S.width || !S.height) return false;
    bool aiming = S.aimOn && S.bodyValid;
    bool follow = !aiming || S.hudFollow;
    Quat q = (follow && S.hudRelValid) ? Mul(S.refQ, S.hudRel) : S.refQ;
    float scale = (aiming && S.hudFollow) ? S.hudFollowScale : g_cfg.hudScale;
    float d = S.hudDistance;
    Vec3 center = S.refP + Rotate(q, { 0, 0, -d });
    float hw = d * S.gameTanX * scale, hh = d * S.gameTanY * scale;

    const XrView& eye = S.views[e];
    Quat inv = Conj(ToQuat(eye.pose.orientation));
    Vec3 ep = ToVec(eye.pose.position);
    float l = tanf(eye.fov.angleLeft), r = tanf(eye.fov.angleRight);
    float u = tanf(eye.fov.angleUp), dn = tanf(eye.fov.angleDown);
    const float corner[4][4] = { { -hw, hh, 0, 0 }, { hw, hh, 1, 0 }, { -hw, -hh, 0, 1 }, { hw, -hh, 1, 1 } };
    for (int i = 0; i < 4; i++) {
        Vec3 p = Rotate(inv, center + Rotate(q, { corner[i][0], corner[i][1], 0 }) - ep);
        if (p.z > -0.01f) return false;     // behind the eye
        float iz = 1.0f / -p.z;
        float nx = (2 * p.x * iz - (r + l)) / (r - l);
        float ny = (2 * p.y * iz - (u + dn)) / (u - dn);
        verts[i][0] = (nx * 0.5f + 0.5f) * S.width - 0.5f;
        verts[i][1] = (0.5f - ny * 0.5f) * S.height - 0.5f;
        verts[i][2] = 0.0f;
        verts[i][3] = iz;
        verts[i][4] = corner[i][2];
        verts[i][5] = corner[i][3];
    }
    return true;
}

bool HudOverlay(IDirect3DTexture9** tex, float verts[4][6])
{
    // XR API: no compositor layers, so the HUD panel is drawn into each eye image.
    if (!S.xrapi || !S.hudTex || !S.hudDrawn || S.frameScope || S.xrSceneFrames < 2) return false;
    if (!HudPanelVerts(S.curEye, verts)) return false;
    *tex = S.hudTex;
    return true;
}

bool MirrorFrameInfo(MirrorFrame* out)
{
    if (S.xrapi || !S.ok || !S.running || !S.frameOpen || !S.viewsValid) return false;
    bool zoomed = S.frameScope && S.anglesThisFrame;
    if (zoomed || S.frameSceneDraws < (unsigned)g_cfg.minSceneDraws) return false;
    const XrFovf& f = S.views[S.curEye].fov;
    out->eye = S.curEye;
    out->left = tanf(-f.angleLeft);
    out->right = tanf(f.angleRight);
    out->up = tanf(f.angleUp);
    out->down = tanf(-f.angleDown);
    out->hud = S.hudDrawn;
    // Where the headset shows the HUD panel in this eye (this frame's head pose: after Present
    // the next frame's views are already in place). Without the game camera this frame the
    // VR layer is placed differently, so the mirror then draws the HUD over the whole window.
    out->hudPanel = S.hudDrawn && S.gameAnglesValid && HudPanelVerts(S.curEye, out->hudVerts);
    return out->left > 0 && out->right > 0 && out->up > 0 && out->down > 0;
}

IDirect3DTexture9* HudTexture()
{
    if (S.hudTex) S.hudTex->AddRef();
    return S.hudTex;
}

IDirect3DSurface9* HudSurface()
{
    if (!S.ok || !S.running || !S.frameOpen || !S.hudTex || (!S.hud.handle && !S.xrapi)) return nullptr;
    IDirect3DSurface9* s = nullptr;
    S.hudTex->GetSurfaceLevel(0, &s);
    return s;
}

void NoteHudDrawn()
{
    S.hudDrawn = true;
}

void NoteInGameMenu(bool seen)
{
    // A few frames in a row so a single stray draw does not switch the controls.
    S.inGameMenuStreak = seen ? S.inGameMenuStreak + 1 : 0;
    if (S.inGameMenuStreak >= 2) S.inGameMenuTick = GetTickCount();
}

void NoteMenuCursor(bool found, float x, float y, float spaceW, float spaceH)
{
    // A cursor drawn for a few frames in a row means a menu is open (also over gameplay).
    S.cursorStreak = found ? S.cursorStreak + 1 : 0;
    if (S.cursorStreak >= 3) S.menuUiTick = GetTickCount();
    S.cursorValid = found;
    if (!found) { S.menuPrevValid = false; return; }
    S.cursorX = x;
    S.cursorY = y;
    if (spaceW > 100 && spaceH > 100) {
        S.cursorSpaceW = spaceW;
        S.cursorSpaceH = spaceH;
    }
}

void Shutdown()
{
    static bool done = false;
    if (done || !S.instance) return;
    done = true;
    input::ReleaseAll();
    // Destroying the instance can block inside some runtimes; never wait for it for long.
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (S.session) {
            if (S.running) xrEndSession(S.session);
            xrDestroySession(S.session);
        }
        xrDestroyInstance(S.instance);
        return 0;
    }, nullptr, 0, nullptr);
    if (th) {
        WaitForSingleObject(th, 1500);
        CloseHandle(th);
    }
    Log("VR shut down");
}

bool SceneActive()
{
    return S.ok && S.running && S.sceneLastPair;
}

bool SpyModelCandidate(const D3DMATRIX& world, unsigned prims)
{
    if (!S.spyCamFrame || !S.curCamMain || !ThirdPerson()) return false;
    // The spy's model: the biggest draw a few meters in front of the camera, near the middle of
    // the view (its parts all share the spy's position as their origin).
    Vec3 t = Row(world, 3);
    Vec3 v = t - S.spyCamPos;
    float ahead = Dot(v, S.spyCamFwd);
    if (ahead <= 30.0f || ahead >= 450.0f || Length(v - S.spyCamFwd * ahead) >= 150.0f || prims <= S.spyBestPrims)
        return false;
    S.spyBestPrims = prims;
    S.spyBestOrigin = t;
    S.spyBestHeadValid = false;
    // The body (not a small part): worth reading its vertices. Under Wine that read stalls the
    // renderer, so with the XR API only while the first-person view needs the head.
    return prims >= 300 && (S.spyFpv || !S.xrapi);
}

void NoteSpyHead(Vec3 top)
{
    // Plausible: the top of the head is a little around the model's origin (eye height).
    if (fabsf(top.z - S.spyBestOrigin.z) > 120.0f) return;
    S.spyBestHead = top;
    S.spyBestHeadValid = true;
}

bool HideDraw(const D3DMATRIX& world, unsigned prims)
{
    // First-person view: the gear hanging on the spy (goggles...) would sit right in front of the
    // eyes. The body itself stays (hands, arms); the eyes are in front of the face.
    if (!S.spyFpv || !S.spyCamFrame || !S.curCamMain || !ThirdPerson() || !S.spyOriginValid) return false;
    if (Length(Row(world, 3) - S.spyOrigin) >= g_cfg.spyFpvHideRadius) return false;
    if (g_cfg.spyFpvShowBody && prims >= 300) return false;
    S.hiddenDraws++;
    // Still part of the 3D scene (a frame with too few scene draws counts as a menu).
    NoteSceneDraw();
    return true;
}

void NoteSceneDraw()
{
    S.frameSceneDraws++;
    if (S.curCamSlot >= 0) S.camSlotDraws[S.curCamSlot]++;
}

void NoteWeaponDraw()
{
    S.lastWeaponTick = GetTickCount();
}

bool LaserQuad(Vec3* corners, D3DMATRIX* view, D3DMATRIX* proj, D3DCOLOR* color)
{
    if (!g_cfg.laser || !S.ok || !S.frameOpen || !S.viewsValid || !S.bodyValid || !S.handValid) return false;
    if (S.frameSceneDraws < (unsigned)g_cfg.minSceneDraws) return false;

    Vec3 start = Row(S.handMat, 3);
    Vec3 dir = Normalize(Row(S.handMat, 2));
    Vec3 end = start + dir * (g_cfg.laserLength * g_cfg.worldScale);
    D3DMATRIX eyeToBody;
    if (!Inverse(S.eyeH[S.curEye], &eyeToBody)) return false;
    Vec3 eye = Row(eyeToBody, 3);
    Vec3 side = Normalize(Cross(dir, eye - start)) * (g_cfg.laserWidth * 0.5f * g_cfg.worldScale);
    corners[0] = start - side;
    corners[1] = start + side;
    corners[2] = end - side;
    corners[3] = end + side;
    *view = S.eyeH[S.curEye];
    *proj = S.eyeProj[S.curEye];
    *color = D3DCOLOR_ARGB(170, 255, 40, 30);
    return true;
}

bool WeaponWorld(const D3DMATRIX& world, const D3DMATRIX& gameView, D3DMATRIX* out)
{
    if (!S.ok || !S.frameOpen || !S.aimOn || !S.bodyValid || !S.handValid || !S.baseValid) return false;
    D3DMATRIX baseInv;
    if (!Inverse(S.baseView, &baseInv)) return false;

    // object -> game camera space -> grip at origin -> hand -> body camera space -> world
    D3DMATRIX grip = Identity();
    grip._41 = -g_cfg.weaponOffsetX;
    grip._42 = -g_cfg.weaponOffsetY;
    grip._43 = -g_cfg.weaponOffsetZ;
    float a = g_cfg.weaponPitch / kDeg;
    D3DMATRIX tilt = Identity();
    tilt._22 = cosf(a); tilt._23 = -sinf(a);
    tilt._32 = sinf(a); tilt._33 = cosf(a);
    D3DMATRIX m = Mul(Mul(world, gameView), grip);
    m = Mul(Mul(m, tilt), S.handMat);
    *out = Mul(m, baseInv);
    return true;
}

namespace {

// Called with the first game camera of every frame: binoculars / sniper mode narrow its FOV.
void UpdateGameZoom()
{
    float fovX = 2 * atanf(S.gameTanX) * kDeg;
    // Zoom starts below ScopeMaxFov and ends when the FOV is clearly wider again on two frames
    // in a row (the game shows a single wider frame while switching the zoom on).
    bool wide = fovX >= g_cfg.scopeMaxFov + 10.0f;
    S.wideFrames = wide ? S.wideFrames + 1 : 0;
    bool zoomed = S.gameZoomed ? S.wideFrames < 2 : fovX < g_cfg.scopeMaxFov;
    // Normal FOV (sizes the zoom screen): only clearly unzoomed frames, not the zoom animation.
    if (!zoomed && wide) {
        if (!S.normalFovValid || fabsf(S.gameTanX - S.normalTanX) > 0.1f * S.normalTanX)
            Log("Game camera FOV %.1f x %.1f deg", fovX, 2 * atanf(S.gameTanY) * kDeg);
        S.normalTanX = S.gameTanX;
        S.normalTanY = S.gameTanY;
        S.normalFovValid = true;
    }
    if (zoomed != S.gameZoomed)
        Log("Game zoom %s (FOV %.1f deg)%s", zoomed ? "ON" : "off", fovX,
            zoomed && g_cfg.scopeView ? " - showing the zoom screen" : "");
    S.gameZoomed = zoomed;
    S.frameScope = zoomed && g_cfg.scopeView;
}

} // namespace

bool EyeTransform(const D3DMATRIX& view, const D3DMATRIX& proj, D3DMATRIX* outView, D3DMATRIX* outProj)
{
    if (!S.ok || !S.frameOpen || !S.viewsValid || !S.frameState.shouldRender) return false;
    if (!IsPerspective(proj)) return false;

    D3DMATRIX base = view;
    bool mainCam = true;
    if (g_cfg.ignoreGamePitch) {
        // Rebuild the game camera with yaw only: head tracking provides pitch and roll.
        D3DMATRIX cam;
        if (Inverse(view, &cam)) {
            Vec3 up = { 0, 0, 0 };
            (&up.x)[g_cfg.worldUpAxis] = 1.0f;
            Vec3 r = Row(cam, 0), u = Row(cam, 1), f = Row(cam, 2), pos = Row(cam, 3);
            float sr = Length(r), su = Length(u), sf = Length(f);
            if (Dot(u, up) < 0) up = up * -1.0f;
            Vec3 ff = f - up * Dot(f, up);
            if (Length(ff) > 1e-3f * sf) {
                ff = Normalize(ff);
                S.lastFlatFwd = ff;
                S.haveLastLevel = true;
            } else if (S.haveLastLevel) {
                ff = S.lastFlatFwd;
            } else {
                ff = Normalize(f);
            }
            float handed = Dot(Cross(up, ff), r) < 0 ? -1.0f : 1.0f;

            // The first camera of a frame is the main view (later ones can be e.g. the weapon's own FOV).
            if (!S.anglesThisFrame) {
                S.anglesThisFrame = true;
                S.gameYaw = atan2f(ff.y, ff.x);
                S.gamePitch = asinf(fmaxf(-1.0f, fminf(1.0f, Dot(f, up) / sf)));
                S.gameTanX = proj._34 / proj._11;
                S.gameTanY = proj._34 / proj._22;
                S.gameAnglesValid = true;
                UpdateGameZoom();
            }

            // Which camera is this: count draws per position; the main view is the one near
            // last frame's busiest camera (the skybox camera sits far away in the sky zone).
            S.curCamSlot = -1;
            for (int i = 0; i < S.camSlotCount; i++)
                if (Length(S.camSlotPos[i] - pos) < 1.0f) S.curCamSlot = i;
            if (S.curCamSlot < 0 && S.camSlotCount < State::kCamSlots) {
                S.curCamSlot = S.camSlotCount++;
                S.camSlotPos[S.curCamSlot] = pos;
                S.camSlotDraws[S.curCamSlot] = 0;
            }
            mainCam = !S.mainCamValid || Length(pos - S.mainCamPos) < 400.0f;
            if (mainCam && ThirdPerson() && !S.spyCamFrame) {
                // The game camera the spy model is searched in front of (HideDraw).
                S.spyCamFrame = true;
                S.spyCamPos = pos;
                S.spyCamFwd = Normalize(f);
            }

            // Aim mode: the view turns with the body yaw, not with the (controller-driven) game camera.
            bool steered = S.aimOn && S.bodyValid && g_cfg.worldUpAxis == 2;
            if (steered)
                ff = { cosf(S.bodyYaw), sinf(S.bodyYaw), 0.0f };

            Vec3 rr = Normalize(Cross(up, ff)) * handed;

            // Third person: place the VR view relative to the spy (see State). Only the main
            // camera: moving the skybox camera inside its small-scale sky zone would make the sky
            // swing wildly.
            S.curCamMain = mainCam;
            if (mainCam && ThirdPerson() && S.spyOriginValid) {
                S.spyAxes[0] = rr; S.spyAxes[1] = up; S.spyAxes[2] = ff;
                if (S.spyFpv) {
                    // The spy's eyes, attached to the top of the head (follows crouching etc.).
                    pos = SpyEyeBase() + rr * S.eyeRight + up * S.eyeUp + ff * S.eyeFwd;
                } else if (steered && g_cfg.spyStableCamera) {
                    // Camera relative to the spy in the game camera's axes, re-applied with the
                    // level body-yaw axes: the same spot behind the spy, without the orbit swing.
                    Vec3 d = pos - S.spyOrigin;
                    pos = S.spyOrigin + rr * Dot(d, Normalize(r)) + up * Dot(d, Normalize(u)) + ff * Dot(d, Normalize(f));
                }
            }

            // Spy binoculars put the game camera at the spy's eyes: measure where they are
            // relative to the spy's position seen just before (once the zoom has settled).
            if (mainCam && S.frameScope && S.scopeHead && !S.eyeCalibDone && S.spyOriginValid &&
                GetTickCount() - S.zoomStartTick > 300) {
                S.eyeCalibDone = true;
                Vec3 d = pos - SpyEyeBase();
                if (Length(d) < 200.0f) {
                    // Height and side only: the view stays SpyEyeForward in front of the head, so
                    // the goggles and the face are behind it.
                    S.eyeRight = Dot(d, S.spyAxes[0]);
                    S.eyeUp = Dot(d, S.spyAxes[1]);
                    Log("Spy eyes measured with the binoculars: SpyEyeRight=%.0f SpyEyeUp=%.0f (forward %.0f, kept at %.0f)",
                        S.eyeRight, S.eyeUp, Dot(d, S.spyAxes[2]), S.eyeFwd);
                } else {
                    Log("Spy eye measurement skipped (binocular camera %.0f UU from the spy - moved?)", Length(d));
                }
            }
            D3DMATRIX level = Identity();
            SetRow(level, 0, rr * sr, 0);
            SetRow(level, 1, up * su, 0);
            SetRow(level, 2, ff * sf, 0);
            SetRow(level, 3, pos, 1);
            D3DMATRIX v;
            if (Inverse(level, &v)) base = v;
        }
    }
    // Zoomed: the whole frame keeps the game's camera and is shown on the zoom screen.
    if (S.frameScope) return false;
    S.baseView = base;
    S.baseValid = true;

    // HUD panel orientation: the game camera's axes expressed in the rendered (body) camera
    // space, taken from the same frame's matrices.
    if (!S.hudRelThisFrame) {
        D3DMATRIX gameCam;
        if (Inverse(view, &gameCam)) {
            auto toBody = [&](Vec3 v) {
                return Normalize(Vec3{ v.x * base._11 + v.y * base._21 + v.z * base._31,
                                       v.x * base._12 + v.y * base._22 + v.z * base._32,
                                       v.x * base._13 + v.y * base._23 + v.z * base._33 });
            };
            Vec3 bx = toBody(Row(gameCam, 0)), by = toBody(Row(gameCam, 1)), bz = toBody(Row(gameCam, 2));
            // D3D view space (left-handed, +z forward) -> OpenXR (right-handed, -z forward)
            Vec3 X = { bx.x, bx.y, -bx.z }, Y = { by.x, by.y, -by.z }, F = { bz.x, bz.y, -bz.z };
            S.hudRel = FromBasis(X, Y, F * -1.0f);
            S.hudRelValid = true;
            S.hudRelThisFrame = true;
        }
    }
    // The skybox (any camera far from the main one) is a small-scale model in the sky zone:
    // eye separation and head movement in it look huge, so it gets the head's rotation only and
    // appears infinitely far away, as a real sky does.
    D3DMATRIX H = S.eyeH[S.curEye];
    if (!mainCam) H._41 = H._42 = H._43 = 0.0f;
    *outView = Mul(base, H);

    // Same depth mapping as the game, x/y replaced by the eye's (asymmetric) field of view.
    const XrFovf& fov = S.views[S.curEye].fov;
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float u = tanf(fov.angleUp), d = tanf(fov.angleDown);
    float k = proj._34;
    D3DMATRIX p = proj;
    for (int i = 0; i < 4; i++) p.m[i][0] = p.m[i][1] = 0.0f;
    p._11 = 2.0f / (r - l) * k;
    p._31 = -(r + l) / (r - l) * k;
    p._22 = 2.0f / (u - d) * k;
    p._32 = -(u + d) / (u - d) * k;
    *outProj = p;

    S.eyeProj[S.curEye] = p;
    return true;
}

} // namespace vr
