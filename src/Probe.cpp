// Diagnostic only. No NGX contexts or GPU submissions are created here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <IUnityGraphics.h>
#include <IUnityGraphicsVulkan.h>
#include <mutex>
#include <sstream>
#include <string>
#include <cstring>
#include <atomic>
#include <cstdio>

static IUnityInterfaces* unity = nullptr;
static IUnityGraphicsVulkan* vulkan = nullptr;
static IUnityGraphics* graphics = nullptr;
static int probeEventId = -1;
// Bits: PluginLoad seen, graphics interface, Vulkan interface, event ID, device initialized.
static std::atomic<int> loadStatus{0};
static std::mutex stateMutex;
static bool pending = false, ready = false;
static void* textures[3]{};
static std::string report;
#define API extern "C" __declspec(dllexport)

static void ConfigureVulkan()
{
    vulkan = unity ? unity->Get<IUnityGraphicsVulkan>() : nullptr;
    if (!graphics || !vulkan) return;
    if (probeEventId < 0) probeEventId = graphics->ReserveEventIDRange(1);
    UnityVulkanPluginEventConfig config{};
    config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
    config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
    config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission;
    if (probeEventId >= 0) vulkan->ConfigureEvent(probeEventId, &config);
    loadStatus.fetch_or(4 | (probeEventId >= 0 ? 8 : 0));
}

#ifdef VF_SMOKE_TEST
// Test-only binary, never included in bin/. Exits before managed game code or worlds load.
static void SmokeLog(const char* stage)
{
    wchar_t path[4096]{};
    DWORD length = GetEnvironmentVariableW(L"VF_SMOKE_LOG", path, 4096);
    if (length == 0 || length >= 4096) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a") != 0 || !file) return;
    std::fprintf(file, "%s status=%d eventId=%d renderer=%d\n", stage, loadStatus.load(),
        probeEventId, graphics ? static_cast<int>(graphics->GetRenderer()) : -1);
    std::fclose(file);
}
#endif

static void UNITY_INTERFACE_API DeviceEvent(UnityGfxDeviceEventType event)
{
    if (event == kUnityGfxDeviceEventInitialize && graphics &&
        graphics->GetRenderer() == kUnityGfxRendererVulkan)
    {
        ConfigureVulkan();
        loadStatus.fetch_or(16);
#ifdef VF_SMOKE_TEST
        SmokeLog("DEVICE_INITIALIZED");
        TerminateProcess(GetCurrentProcess(), graphics && vulkan ? 0 : 4);
#endif
    }
    if (event == kUnityGfxDeviceEventShutdown)
    {
        vulkan = nullptr;
        loadStatus.fetch_and(~(4 | 8 | 16));
    }
}

API void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces)
{
    if (unity == interfaces && graphics) return;
    loadStatus.store(1);
    unity = interfaces;
    if (!unity) return;
    graphics = interfaces->Get<IUnityGraphics>();
    if (graphics)
    {
        loadStatus.fetch_or(2);
        graphics->RegisterDeviceEventCallback(DeviceEvent);
        // Late loading misses the initialize event; the renderer query is documented thread-safe.
        // Preloaded plugins run before a graphics device exists: ConfigureEvent must wait for it.
        if (graphics->GetRenderer() == kUnityGfxRendererVulkan) DeviceEvent(kUnityGfxDeviceEventInitialize);
    }
#ifdef VF_SMOKE_TEST
    SmokeLog("PLUGIN_LOADED");
#endif
}

API void UNITY_INTERFACE_API UnityPluginUnload()
{
    if (graphics) graphics->UnregisterDeviceEventCallback(DeviceEvent);
    graphics = nullptr;
    vulkan = nullptr;
    unity = nullptr;
    probeEventId = -1;
    loadStatus.store(0);
}

API int __cdecl VF_Begin(void* color, void* depth, void* motion)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (pending || ready) return 0;
    textures[0] = color; textures[1] = depth; textures[2] = motion;
    pending = true;
    return 1;
}

static void UNITY_INTERFACE_API RenderEvent(int)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!pending) return;
    std::ostringstream out;
    if (!vulkan || !graphics || graphics->GetRenderer() != kUnityGfxRendererVulkan)
        out << "BLOCKED NO_UNITY_VULKAN";
    else
    {
        auto instance = vulkan->Instance();
        auto gdpa = instance.getInstanceProcAddr ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            instance.getInstanceProcAddr(instance.instance, "vkGetDeviceProcAddr")) : nullptr;
        bool memory = gdpa && gdpa(instance.device, "vkGetMemoryWin32HandlePropertiesKHR");
        bool semaphore = gdpa && gdpa(instance.device, "vkImportSemaphoreWin32HandleKHR");
        out << "renderThread=" << GetCurrentThreadId()
            << " memoryWin32Proc=" << memory << " semaphoreWin32Proc=" << semaphore;
        // Non-null pointers are a prerequisite, not proof that extensions were enabled.
        if (!memory || !semaphore) out << " BLOCKED EXTERNAL_WIN32_ENTRYPOINT_MISSING";
        else out << " EXTERNAL_EXTENSION_ENABLEMENT_UNVERIFIED";
        const char* names[] = {"color", "depth", "motion"};
        for (int i = 0; i < 3; ++i)
        {
            UnityVulkanImage image{};
            bool ok = textures[i] && vulkan->AccessTexture(textures[i], nullptr,
                VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
                kUnityVulkanResourceAccess_ObserveOnly, &image);
            out << " " << names[i] << "Access=" << ok;
            if (ok) out << "[" << image.extent.width << "x" << image.extent.height
                << ",format=" << image.format << ",layout=" << image.layout
                << ",usage=" << image.usage << ",samples=" << image.samples << "]";
        }
        // Resource access invalidates prior recording state, query it last.
        UnityVulkanRecordingState recording{};
        bool recordingOk = vulkan->CommandRecordingState(&recording,
            kUnityVulkanGraphicsQueueAccess_DontCare);
        out << " commandState=" << recordingOk;
        if (recordingOk)
        {
            out << " level=" << recording.commandBufferLevel
                << " frame=" << recording.currentFrameNumber
                << " safeFrame=" << recording.safeFrameNumber;
            if (recording.commandBufferLevel != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
                out << " BLOCKED SECONDARY_COMMAND_BUFFER";
        }
    }
    HMODULE opti = nullptr;
    for (const auto* name : {L"dxgi.dll", L"OptiScaler.dll", L"version.dll", L"winmm.dll", L"nvngx.dll"})
    {
        auto module = GetModuleHandleW(name);
        if (module && GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1")) { opti = module; break; }
    }
    // Export presence alone does not establish OptiScaler identity or source commit.
    out << " NGX_PROVIDER_MODULE=" << (opti ? "PRESENT_IDENTITY_UNVERIFIED" : "NOT_FOUND")
        << " input=NOT_ATTEMPTED evaluations=0 fsr4=NOT_VERIFIED native1440=NOT_VERIFIED";
    report = out.str();
    for (auto& texture : textures) texture = nullptr;
    pending = false;
    ready = true;
}

API UnityRenderingEvent __cdecl VF_GetEvent() { return RenderEvent; }
API int __cdecl VF_GetEventId() { return probeEventId; }
API int __cdecl VF_GetLoadStatus() { return loadStatus.load(); }
API int __cdecl VF_GetVersion() { return 101; }

API int __cdecl VF_Read(char* destination, int capacity)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (!ready || !destination || capacity <= 0) return 0;
    if (report.size() + 1 > static_cast<size_t>(capacity)) return -1;
    std::memcpy(destination, report.c_str(), report.size() + 1);
    ready = false;
    return 1;
}
