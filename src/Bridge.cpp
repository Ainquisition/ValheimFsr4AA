// Vulkan NGX bridge; used by isolated checks and the adapter candidate.
#define UnityPluginLoad BaseUnityPluginLoad
#define UnityPluginUnload BaseUnityPluginUnload
#include "Probe.cpp"
#undef UnityPluginLoad
#undef UnityPluginUnload

#ifdef VF_NGX_PROBE
#include <nvsdk_ngx_vk.h>
static HMODULE opti = nullptr;
static DWORD optiLoadError = 0;
static NVSDK_NGX_Parameter* sessionParams = nullptr;
static NVSDK_NGX_Handle* sessionHandle = nullptr;
static UnityVulkanInstance sessionDevice{};
static bool sessionInitialized = false, gpuUnsafe = false;
static bool normalizedCoordinates = false;
struct AdapterMetadata { float jitterX, jitterY, nearPlane, farPlane, fov, deltaMs; unsigned reset; };
static AdapterMetadata metadata{0, 0, 0.1f, 1000.0f, 1.04719755f, 16.67f, 1};
API void __cdecl VF_AdapterMetadata(AdapterMetadata value) { metadata = value; }
API int __cdecl VF_AdapterHealthy() { return opti && !gpuUnsafe ? 1 : 0; }
static void ReleaseSession()
{
    if (!opti || gpuUnsafe) return;
    auto release = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_ReleaseFeature)>(GetProcAddress(opti, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    auto destroy = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_DestroyParameters)>(GetProcAddress(opti, "NVSDK_NGX_VULKAN_DestroyParameters"));
    auto shutdown = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_Shutdown1)>(GetProcAddress(opti, "NVSDK_NGX_VULKAN_Shutdown1"));
    if (sessionHandle && release) release(sessionHandle);
    if (sessionParams && destroy) destroy(sessionParams);
    if (sessionInitialized && shutdown) shutdown(sessionDevice.device);
    sessionHandle = nullptr; sessionParams = nullptr; sessionInitialized = false;
}
static void* ngxTextures[4]{};
static UnityVulkanImage ngxImages[4]{};
static bool ngxPrepared = false;
API void __cdecl VF_TestNgxTextures(void* color, void* depth, void* motion, void* output)
{
    ngxTextures[0] = color; ngxTextures[1] = depth; ngxTextures[2] = motion; ngxTextures[3] = output;
}
static bool ProbeNgx(VkCommandBuffer buffer, std::ostringstream& out)
{
    if (!opti) { out << " NGX_LOAD_ERROR=" << optiLoadError; return false; }
#define NGX_PROC(name) auto name = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_##name)>(GetProcAddress(opti, "NVSDK_NGX_VULKAN_" #name))
    NGX_PROC(Init_Ext2); NGX_PROC(AllocateParameters); NGX_PROC(CreateFeature1);
    NGX_PROC(ReleaseFeature); NGX_PROC(DestroyParameters); NGX_PROC(Shutdown1);
    NGX_PROC(EvaluateFeature);
#undef NGX_PROC
    if (!Init_Ext2 || !AllocateParameters || !CreateFeature1 || !ReleaseFeature || !DestroyParameters || !Shutdown1 || !EvaluateFeature)
    { out << " NGX_EXPORTS_MISSING"; return false; }
    auto instance = vulkan->Instance();
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance.getInstanceProcAddr(instance.instance, "vkGetDeviceProcAddr"));
    const auto init = sessionInitialized ? NVSDK_NGX_Result_Success : Init_Ext2(0, L".", instance.instance, instance.physicalDevice, instance.device,
        instance.getInstanceProcAddr, gdpa, NVSDK_NGX_Version_API, nullptr);
    out << " NGX_init=" << std::hex << static_cast<unsigned>(init);
    if (NVSDK_NGX_FAILED(init)) return false;
    sessionInitialized = true; sessionDevice = instance;
    bool ended = false;
    const auto allocated = sessionParams ? NVSDK_NGX_Result_Success : AllocateParameters(&sessionParams);
    auto params = sessionParams;
    out << " NGX_params=" << static_cast<unsigned>(allocated);
    if (NVSDK_NGX_SUCCEED(allocated) && params)
    {
        params->Set(NVSDK_NGX_Parameter_Width, 2560u);
        params->Set(NVSDK_NGX_Parameter_Height, 1440u);
        params->Set(NVSDK_NGX_Parameter_OutWidth, 2560u);
        params->Set(NVSDK_NGX_Parameter_OutHeight, 1440u);
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
            static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                             NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
        const auto created = sessionHandle ? NVSDK_NGX_Result_Success : CreateFeature1(instance.device, buffer, NVSDK_NGX_Feature_SuperSampling, params, &sessionHandle);
        auto handle = sessionHandle;
        out << " NGX_create=" << static_cast<unsigned>(created) << " handle=" << (handle != nullptr);
        VkImageView views[4]{};
        auto createView = reinterpret_cast<PFN_vkCreateImageView>(gdpa(instance.device, "vkCreateImageView"));
        auto destroyView = reinterpret_cast<PFN_vkDestroyImageView>(gdpa(instance.device, "vkDestroyImageView"));
        bool viewsReady = ngxPrepared && createView && destroyView;
        NVSDK_NGX_Resource_VK resources[4]{};
        for (int i = 0; viewsReady && i < 4; ++i)
        {
            const auto& image = ngxImages[i];
            VkImageViewCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = image.image; info.viewType = VK_IMAGE_VIEW_TYPE_2D; info.format = image.format;
            info.subresourceRange = {image.aspect, 0, 1, 0, 1};
            const auto result = createView(instance.device, &info, nullptr, &views[i]);
            out << " view" << i << "=" << result;
            viewsReady = result == VK_SUCCESS;
            auto& resource = resources[i];
            resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
            resource.ReadWrite = i == 3;
            resource.Resource.ImageViewInfo.Image = image.image;
            resource.Resource.ImageViewInfo.ImageView = views[i];
            resource.Resource.ImageViewInfo.Format = image.format;
            resource.Resource.ImageViewInfo.Width = image.extent.width;
            resource.Resource.ImageViewInfo.Height = image.extent.height;
            resource.Resource.ImageViewInfo.SubresourceRange = info.subresourceRange;
        }
        if (NVSDK_NGX_SUCCEED(created) && handle && viewsReady)
        {
            // The tracker ignores OnBegin until currentFeature exists. Restart our empty buffer after CreateFeature.
            auto finishEmpty = reinterpret_cast<PFN_vkEndCommandBuffer>(gdpa(instance.device, "vkEndCommandBuffer"));
            auto reset = reinterpret_cast<PFN_vkResetCommandBuffer>(gdpa(instance.device, "vkResetCommandBuffer"));
            auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(gdpa(instance.device, "vkBeginCommandBuffer"));
            VkCommandBufferBeginInfo beginInfo{}; beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (!finishEmpty || !reset || !begin || finishEmpty(buffer) != VK_SUCCESS ||
                reset(buffer, 0) != VK_SUCCESS || begin(buffer, &beginInfo) != VK_SUCCESS)
            { out << " BLOCKED_RESTART_BUFFER"; gpuUnsafe = true; return true; }
            params->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(&resources[0]));
            params->Set(NVSDK_NGX_Parameter_Depth, static_cast<void*>(&resources[1]));
            params->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void*>(&resources[2]));
            params->Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(&resources[3]));
            params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 2560u);
            params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 1440u);
            params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, metadata.jitterX);
            params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, metadata.jitterY);
            params->Set(NVSDK_NGX_Parameter_MV_Scale_X, -2560.0f);
            params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, normalizedCoordinates ? 1440.0f : -1440.0f);
            params->Set(NVSDK_NGX_Parameter_Reset, metadata.reset);
            params->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, metadata.deltaMs);
            params->Set("FSR.cameraNear", metadata.farPlane);
            params->Set("FSR.cameraFar", metadata.nearPlane);
            params->Set("FSR.cameraFovAngleVertical", metadata.fov);
            params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
            params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
            LARGE_INTEGER started{}, finished{}, frequency{};
            QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
            const auto evaluated = EvaluateFeature(buffer, handle, params, nullptr);
            out << " NGX_evaluate=" << static_cast<unsigned>(evaluated);
            auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(gdpa(instance.device, "vkEndCommandBuffer"));
            auto createFence = reinterpret_cast<PFN_vkCreateFence>(gdpa(instance.device, "vkCreateFence"));
            auto submit = reinterpret_cast<PFN_vkQueueSubmit>(gdpa(instance.device, "vkQueueSubmit"));
            auto wait = reinterpret_cast<PFN_vkWaitForFences>(gdpa(instance.device, "vkWaitForFences"));
            auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(gdpa(instance.device, "vkDestroyFence"));
            VkFence fence = VK_NULL_HANDLE;
            VkFenceCreateInfo fenceInfo{}; fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            // On a failed GPU transaction retain potentially live resources; do not terminate the game.
            if (!end || !createFence || !submit || !wait || !destroyFence ||
                end(buffer) != VK_SUCCESS || createFence(instance.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
            { out << " BLOCKED_END_OR_FENCE"; gpuUnsafe = true; return true; }
            ended = true;
            VkSubmitInfo submission{}; submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submission.commandBufferCount = 1; submission.pCommandBuffers = &buffer;
            const auto submitted = submit(instance.graphicsQueue, 1, &submission, fence);
            out << " queueSubmit=" << submitted;
            if (submitted != VK_SUCCESS) { gpuUnsafe = true; return true; }
            const auto completed = wait(instance.device, 1, &fence, VK_TRUE, 5000000000ull);
            out << " fenceWait=" << completed;
            if (completed != VK_SUCCESS) { gpuUnsafe = true; return true; }
            QueryPerformanceCounter(&finished);
            out << " evaluateThroughFenceWallMs=" << (1000.0 * static_cast<double>(finished.QuadPart - started.QuadPart) / frequency.QuadPart);
            destroyFence(instance.device, fence, nullptr);
        }
        for (auto view : views) if (view) destroyView(instance.device, view, nullptr);
        if (NVSDK_NGX_FAILED(created)) ReleaseSession();
    }
    out << " contextPersistent=1" << std::dec;
    return ended;
}
#endif

static int testBase = -1;
#ifdef VF_NGX_PROBE
static constexpr int frameSlotCount = 3;
struct FrameData { void* textures[4]; AdapterMetadata camera; unsigned normalized; };
static_assert(sizeof(FrameData) == 64, "Managed FrameData ABI mismatch");
struct FrameSlot
{
    FrameData data{};
    UnityVulkanImage images[4]{};
    int state = 0; // 0 free, 1 queued, 2 processing, 3 completed.
    bool prepared = false;
    std::string report;
};
static FrameSlot frameSlots[frameSlotCount];
static std::mutex frameMutex;
static int frameEventBase = -1;
API int __cdecl VF_FrameVersion() { return 201; }
API int __cdecl VF_FrameEventId(int slot) { return slot >= 0 && slot < frameSlotCount && frameEventBase >= 0 ? frameEventBase + slot : -1; }
API int __cdecl VF_FrameBegin(int slot, const FrameData* data)
{
    if (!data || slot < 0 || slot >= frameSlotCount || frameEventBase < 0 || !vulkan) return 0;
    std::lock_guard<std::mutex> lock(frameMutex);
    auto& frame = frameSlots[slot];
    if (frame.state != 0) return 0;
    frame.data = *data;
    frame.report.clear();
    frame.state = 1;
    return 1;
}
API int __cdecl VF_FrameRead(int slot, char* output, int capacity)
{
    if (slot < 0 || slot >= frameSlotCount || !output || capacity <= 0) return 0;
    std::lock_guard<std::mutex> lock(frameMutex);
    auto& frame = frameSlots[slot];
    if (frame.state != 3 || frame.report.size() + 1 > static_cast<size_t>(capacity)) return 0;
    std::memcpy(output, frame.report.c_str(), frame.report.size() + 1);
    frame.state = 0;
    return 1;
}
#endif
static void UNITY_INTERFACE_API TestDeviceEvent(UnityGfxDeviceEventType event)
{
#ifdef VF_NGX_PROBE
    if (event == kUnityGfxDeviceEventShutdown) ReleaseSession();
    if (event == kUnityGfxDeviceEventInitialize && graphics && vulkan && graphics->GetRenderer() == kUnityGfxRendererVulkan)
    {
        if (frameEventBase < 0) frameEventBase = graphics->ReserveEventIDRange(frameSlotCount);
        UnityVulkanPluginEventConfig config{};
        config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
        config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
        config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission;
        for (int i = 0; i < frameSlotCount; ++i) vulkan->ConfigureEvent(frameEventBase + i, &config);
    }
#endif
#ifdef VF_COMMAND_TEST
    if (event != kUnityGfxDeviceEventInitialize || !graphics || !vulkan ||
        graphics->GetRenderer() != kUnityGfxRendererVulkan || testBase >= 0) return;
    testBase = graphics->ReserveEventIDRange(5);
    const unsigned flags[] = {1, 5, 3, 7, 9};
    for (int i = 0; i < 5; ++i)
    {
        UnityVulkanPluginEventConfig config{};
        config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
        config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
        config.flags = flags[i];
        vulkan->ConfigureEvent(testBase + i, &config);
    }
#endif
}
API void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces)
{
#ifdef VF_NGX_PROBE
    wchar_t path[4096]{};
    DWORD size = GetEnvironmentVariableW(L"VF_OPTI_DLL", path, 4096);
    wchar_t executable[4096]{};
    DWORD executableSize = GetModuleFileNameW(nullptr, executable, 4096);
    std::wstring directory;
    if (executableSize > 0 && executableSize < 4096)
    {
        directory = executable;
        directory.resize(directory.find_last_of(L"\\/") + 1);
    }
    // Unity preloads this plugin before Vulkan creation, including mod-manager launches.
    std::wstring runtime = size == 0 ? directory + L"ValheimFsr4AA-runtime\\OptiScaler.dll" : path;
    if (size >= 4096 || directory.empty()) optiLoadError = ERROR_BAD_PATHNAME;
    else
    {
#ifdef VF_LIVE_ADAPTER
        std::wstring proxy = directory + L"dxgi.dll";
        if (GetFileAttributesW(proxy.c_str()) != INVALID_FILE_ATTRIBUTES) optiLoadError = ERROR_ALREADY_EXISTS;
        else
#endif
        { opti = LoadLibraryW(runtime.c_str()); optiLoadError = opti ? 0 : GetLastError(); }
    }
    // Keep OptiScaler loaded until process exit: its Vulkan hooks outlive this test context.
#endif
    BaseUnityPluginLoad(interfaces);
    if (graphics) graphics->RegisterDeviceEventCallback(TestDeviceEvent);
}
API void UNITY_INTERFACE_API UnityPluginUnload()
{
    if (graphics) graphics->UnregisterDeviceEventCallback(TestDeviceEvent);
    BaseUnityPluginUnload();
}
API int __cdecl VF_TestGetEventId(int mode)
{
    return testBase >= 0 && mode >= 0 && mode < 5 ? testBase + mode : -1;
}

static std::mutex queueMutex;
static std::string queueReport;
static void UNITY_INTERFACE_API QueueProbe(int, void*)
{
    std::ostringstream out;
    auto instance = vulkan->Instance();
    UnityVulkanRecordingState state{};
    bool ok = vulkan->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_Allow);
    out << "ACCESS_QUEUE recordingState=" << ok << " unityCommandBuffer=" << (state.commandBuffer != VK_NULL_HANDLE);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance.getInstanceProcAddr(instance.instance, "vkGetDeviceProcAddr"));
    auto create = reinterpret_cast<PFN_vkCreateCommandPool>(gdpa(instance.device, "vkCreateCommandPool"));
    auto destroy = reinterpret_cast<PFN_vkDestroyCommandPool>(gdpa(instance.device, "vkDestroyCommandPool"));
    auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(gdpa(instance.device, "vkAllocateCommandBuffers"));
    auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(gdpa(instance.device, "vkBeginCommandBuffer"));
    auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(gdpa(instance.device, "vkEndCommandBuffer"));
    if (!create || !destroy || !allocate || !begin || !end) out << " BLOCKED missing core Vulkan functions";
    else
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = instance.queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkResult created = create(instance.device, &poolInfo, nullptr, &pool);
        out << " createPool=" << created;
        if (created == VK_SUCCESS)
        {
            VkCommandBuffer buffer = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocation.commandPool = pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            VkResult allocated = allocate(instance.device, &allocation, &buffer);
            out << " allocatePrimary=" << allocated;
            if (allocated == VK_SUCCESS)
            {
                VkCommandBufferBeginInfo info{};
                info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VkResult started = begin(buffer, &info);
                out << " begin=" << started;
                if (started == VK_SUCCESS)
                {
#ifdef VF_NGX_PROBE
                    if (!ProbeNgx(buffer, out)) out << " end=" << end(buffer);
#else
                    out << " end=" << end(buffer);
#endif
                }
            }
            // The NGX path waits for completion. Retain resources after a failed GPU transaction.
#ifdef VF_NGX_PROBE
            if (!gpuUnsafe)
#endif
                destroy(instance.device, pool, nullptr);
        }
    }
#ifdef VF_NGX_PROBE
    out << " inputMapping=REQUIRES_CAMERA_VALIDATION";
#else
    out << " submitted=0 NGX=NOT_TESTED resourceOrdering=NOT_TESTED";
#endif
    std::lock_guard<std::mutex> lock(queueMutex);
    queueReport = out.str();
}
static void UNITY_INTERFACE_API RequestQueueProbe(int)
{
    if (!vulkan)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queueReport = "BLOCKED Unity Vulkan device unavailable";
        return;
    }
#ifdef VF_NGX_PROBE
    ngxPrepared = true;
    for (int i = 0; i < 4; ++i)
    {
        // OptiScaler's NGX contract assumes read-only inputs and a GENERAL output.
        const VkAccessFlags access = i == 3 ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        const VkImageLayout layout = i == 3 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!ngxTextures[i] || !vulkan->AccessTexture(ngxTextures[i], nullptr, layout,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, access, kUnityVulkanResourceAccess_PipelineBarrier, &ngxImages[i]))
            ngxPrepared = false;
        if (ngxImages[i].extent.width != 2560 || ngxImages[i].extent.height != 1440 || ngxImages[i].samples != VK_SAMPLE_COUNT_1_BIT)
            ngxPrepared = false;
        if (i == 1) ngxImages[i].aspect = (VK_IMAGE_ASPECT_DEPTH_BIT & ngxImages[i].aspect) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }
#endif
    vulkan->AccessQueue(QueueProbe, 0, nullptr, true);
}
API UnityRenderingEvent __cdecl VF_TestQueueEvent() { return RequestQueueProbe; }
API int __cdecl VF_TestReadQueue(char* output, int capacity)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    if (queueReport.empty() || capacity <= 0 || queueReport.size() + 1 > static_cast<size_t>(capacity)) return 0;
    std::memcpy(output, queueReport.c_str(), queueReport.size() + 1);
    queueReport.clear();
    return 1;
}

#ifdef VF_NGX_PROBE
static void UNITY_INTERFACE_API ProcessQueuedFrame(int slot, void*)
{
    // AccessQueue serializes access to the graphics queue. Session state is used only inside this callback.
    auto& frame = frameSlots[slot];
    metadata = frame.data.camera;
    normalizedCoordinates = frame.data.normalized != 0;
    ngxPrepared = frame.prepared;
    for (int i = 0; i < 4; ++i) ngxImages[i] = frame.images[i];
    QueueProbe(0, nullptr);
    std::string completed;
    { std::lock_guard<std::mutex> lock(queueMutex); completed = queueReport; queueReport.clear(); }
    std::lock_guard<std::mutex> lock(frameMutex);
    frame.report = "slot=" + std::to_string(slot) + " reset=" + std::to_string(frame.data.camera.reset) + " " + completed;
    frame.state = 3;
}
static void UNITY_INTERFACE_API PrepareQueuedFrame(int eventId)
{
    const int slot = eventId - frameEventBase;
    if (slot < 0 || slot >= frameSlotCount) return;
    auto& frame = frameSlots[slot];
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (frame.state != 1) return;
        frame.state = 2;
    }
    if (!vulkan)
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        frame.report = "BLOCKED Unity Vulkan device unavailable"; frame.state = 3; return;
    }
    frame.prepared = true;
    for (int i = 0; i < 4; ++i)
    {
        const auto access = i == 3 ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        const auto layout = i == 3 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto& image = frame.images[i];
        if (!frame.data.textures[i] || !vulkan->AccessTexture(frame.data.textures[i], nullptr, layout,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, access, kUnityVulkanResourceAccess_PipelineBarrier, &image) ||
            image.extent.width != 2560 || image.extent.height != 1440 || image.samples != VK_SAMPLE_COUNT_1_BIT)
            frame.prepared = false;
        if (i == 1) image.aspect = (image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }
    vulkan->AccessQueue(ProcessQueuedFrame, slot, nullptr, true);
}
API UnityRenderingEvent __cdecl VF_FrameEvent() { return PrepareQueuedFrame; }
#endif
