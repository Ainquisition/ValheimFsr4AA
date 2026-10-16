#include "Probe.cpp"
#include <cassert>
#include <cstring>

static UnityGfxRenderer renderer = kUnityGfxRendererVulkan;
static UnityGfxRenderer UNITY_INTERFACE_API Renderer() { return renderer; }
static int registerCalls = 0, unregisterCalls = 0, configureCalls = 0, reserveCalls = 0;
static IUnityGraphics loadGraphics{};
static IUnityGraphicsVulkan loadVulkan{};
static void UNITY_INTERFACE_API RegisterCallback(IUnityGraphicsDeviceEventCallback) { ++registerCalls; }
static void UNITY_INTERFACE_API UnregisterCallback(IUnityGraphicsDeviceEventCallback) { ++unregisterCalls; }
static int UNITY_INTERFACE_API Reserve(int count) { assert(count == 1); ++reserveCalls; return 11; }
static void UNITY_INTERFACE_API Configure(int id, const UnityVulkanPluginEventConfig*)
{
    assert(renderer == kUnityGfxRendererVulkan); assert(id == 11); ++configureCalls;
}
static IUnityInterface* UNITY_INTERFACE_API GetInterface(UnityInterfaceGUID guid)
{
    if (guid == GetUnityInterfaceGUID<IUnityGraphics>()) return &loadGraphics;
    if (guid == GetUnityInterfaceGUID<IUnityGraphicsVulkan>()) return &loadVulkan;
    return nullptr;
}
static PFN_vkVoidFunction VKAPI_PTR NoDeviceProc(VkDevice, const char*) { return nullptr; }
static PFN_vkVoidFunction VKAPI_PTR InstanceProc(VkInstance, const char* name)
{
    return std::strcmp(name, "vkGetDeviceProcAddr") == 0 ? reinterpret_cast<PFN_vkVoidFunction>(NoDeviceProc) : nullptr;
}
static UnityVulkanInstance UNITY_INTERFACE_API Instance()
{
    UnityVulkanInstance result{};
    result.getInstanceProcAddr = InstanceProc;
    return result;
}
static bool UNITY_INTERFACE_API Recording(UnityVulkanRecordingState* state, UnityVulkanGraphicsQueueAccess)
{
    state->commandBufferLevel = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    return true;
}

int main()
{
    char result[4096]{};
    assert(VF_Read(result, sizeof(result)) == 0);
    assert(VF_Begin(nullptr, nullptr, nullptr) == 1);
    assert(VF_Begin(nullptr, nullptr, nullptr) == 0);
    VF_GetEvent()(1);
    assert(VF_Read(result, sizeof(result)) == 1);
    assert(std::strstr(result, "NO_UNITY_VULKAN") != nullptr);
    assert(VF_Read(result, sizeof(result)) == 0);
    assert(VF_Begin(nullptr, nullptr, nullptr) == 1);
    UnityPluginUnload();
    VF_GetEvent()(1);
    assert(VF_Read(result, sizeof(result)) == 1);
    assert(std::strstr(result, "NO_UNITY_VULKAN") != nullptr);
    IUnityGraphics fakeGraphics{};
    fakeGraphics.GetRenderer = Renderer;
    IUnityGraphicsVulkan fakeVulkan{};
    fakeVulkan.Instance = Instance;
    fakeVulkan.CommandRecordingState = Recording;
    graphics = &fakeGraphics;
    vulkan = &fakeVulkan;
    assert(VF_Begin(nullptr, nullptr, nullptr) == 1);
    VF_GetEvent()(1);
    assert(VF_Read(result, sizeof(result)) == 1);
    assert(std::strstr(result, "EXTERNAL_WIN32_ENTRYPOINT_MISSING") != nullptr);
    assert(std::strstr(result, "SECONDARY_COMMAND_BUFFER") != nullptr);
    assert(std::strstr(result, "evaluations=0") != nullptr);
    assert(std::strstr(result, "colorAccess=0") != nullptr);
    // The test never submits GPU work and owns no Vulkan objects.
    graphics = nullptr;
    vulkan = nullptr;
    // Reproduce preloading: the first device is Null. Configuring Vulkan here crashed the player.
    loadGraphics.GetRenderer = Renderer;
    loadGraphics.RegisterDeviceEventCallback = RegisterCallback;
    loadGraphics.UnregisterDeviceEventCallback = UnregisterCallback;
    loadGraphics.ReserveEventIDRange = Reserve;
    loadVulkan.ConfigureEvent = Configure;
    IUnityInterfaces registry{};
    registry.GetInterface = GetInterface;
    renderer = kUnityGfxRendererNull;
    UnityPluginLoad(&registry);
    assert(registerCalls == 1 && configureCalls == 0 && reserveCalls == 0);
    assert(VF_GetLoadStatus() == 3 && VF_GetEventId() == -1);
    DeviceEvent(kUnityGfxDeviceEventInitialize);
    assert(configureCalls == 0);
    renderer = kUnityGfxRendererVulkan;
    DeviceEvent(kUnityGfxDeviceEventInitialize);
    assert(configureCalls == 1 && reserveCalls == 1);
    assert(VF_GetLoadStatus() == 31 && VF_GetEventId() == 11);
    UnityPluginLoad(&registry);
    assert(registerCalls == 1 && reserveCalls == 1);
    DeviceEvent(kUnityGfxDeviceEventShutdown);
    assert(VF_GetLoadStatus() == 3);
    UnityPluginUnload();
    assert(unregisterCalls == 1 && VF_GetLoadStatus() == 0 && VF_GetEventId() == -1);
}
