using System;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

[BepInPlugin("local.valheim.fsr4aa.diagnostics", "Valheim FSR4 AA Diagnostics", "0.1.1")]
public sealed class Plugin : BaseUnityPlugin
{
    const string Native = "ValheimFsr4AA.Native";
    [DllImport(Native, CallingConvention = CallingConvention.Cdecl)] static extern IntPtr VF_GetEvent();
    [DllImport(Native, CallingConvention = CallingConvention.Cdecl)] static extern int VF_GetEventId();
    [DllImport(Native, CallingConvention = CallingConvention.Cdecl)] static extern int VF_GetLoadStatus();
    [DllImport(Native, CallingConvention = CallingConvention.Cdecl)] static extern int VF_GetVersion();
    [DllImport(Native, CallingConvention = CallingConvention.Cdecl)] static extern int VF_Begin(IntPtr color, IntPtr depth, IntPtr motion);
    [DllImport(Native, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int VF_Read(StringBuilder text, int capacity);
    static Plugin self;
    Harmony harmony;
    ConfigEntry<bool> enabledProbe;
    Camera camera;
    DepthTextureMode addedFlags;
    DepthTextureMode expectedFlags;
    IntPtr callback;
    bool pending, nativeFailed;
    Texture[] heldTextures;
    float nextSample, submittedAt;
    Vector3 previousPosition;
    int width, height;
    readonly StringBuilder message = new StringBuilder(4096);

    void Awake()
    {
        self = this;
        enabledProbe = Config.Bind("Diagnostics", "Enabled", true, "Diagnostics only. Does not enable FSR. F8 toggles sampling.");
        Logger.LogInfo("plugin=LOADED input=NOT_ATTEMPTED evaluations=0 fsr4=NOT_VERIFIED native1440=NOT_VERIFIED");
        Logger.LogInfo($"diagnosticSampling={enabledProbe.Value}; F8 toggles sampling");
        Logger.LogInfo($"Unity={Application.unityVersion} graphics={SystemInfo.graphicsDeviceType} GPU={SystemInfo.graphicsDeviceName} driver={SystemInfo.graphicsDeviceVersion} colorspace={QualitySettings.activeColorSpace} reversedZ={SystemInfo.usesReversedZBuffer} uvTop={SystemInfo.graphicsUVStartsAtTop}");
        try
        {
            int version = VF_GetVersion();
            if (version != 101) throw new InvalidOperationException($"Native version mismatch: {version}, expected 101");
            int status = VF_GetLoadStatus();
            Logger.LogInfo($"nativeVersion={version} pluginLoadSeen={(status & 1) != 0} graphicsInterface={(status & 2) != 0} vulkanInterface={(status & 4) != 0} eventReserved={(status & 8) != 0} deviceInitialized={(status & 16) != 0}");
            if ((status & 31) != 31) throw new InvalidOperationException("PRELOAD_NOT_READY: install the checked globalgamemanagers preload patch and restart Valheim");
        }
        catch (Exception e) { Fail(e); return; }
        var type = AccessTools.TypeByName("UnityEngine.PostProcessing.PostProcessingBehaviour");
        var target = type == null ? null : AccessTools.Method(type, "OnRenderImage", new[] { typeof(RenderTexture), typeof(RenderTexture) });
        if (target == null) { Logger.LogError("BLOCKED PostProcessingBehaviour.OnRenderImage not found; no render hooks installed"); return; }
        harmony = new Harmony("local.valheim.fsr4aa.diagnostics");
        harmony.Patch(target, prefix: new HarmonyMethod(typeof(Plugin), nameof(BeforePostProcessing)));
        harmony.Patch(AccessTools.Method(type, "OnPreCull"), postfix: new HarmonyMethod(typeof(Plugin), nameof(AfterPreCull)));
        Logger.LogInfo("hook=PostProcessingBehaviour.OnRenderImage PREFIX; image and TAA are unchanged; native load/render callback remain UNVERIFIED");
    }

    void Update()
    {
        if (Input.GetKeyDown(KeyCode.F8)) { enabledProbe.Value = !enabledProbe.Value; Logger.LogInfo($"diagnosticSampling={enabledProbe.Value}"); }
        if ((!enabledProbe.Value || nativeFailed) && camera) RestoreCamera();
        if (!pending) return;
        try
        {
            message.Clear();
            if (VF_Read(message, message.Capacity) == 1)
            {
                Logger.LogInfo(message.ToString());
                heldTextures = null;
                pending = false;
            }
            else if (Time.realtimeSinceStartup - submittedAt > 10 && !nativeFailed)
            {
                Logger.LogError("BLOCKED render callback did not acknowledge; no more probes will be submitted");
                nativeFailed = true;
            }
        }
        catch (Exception e) { Fail(e); }
    }

    static void BeforePostProcessing(object __instance, RenderTexture source, RenderTexture destination)
    {
        if (self == null) return;
        try { self.Sample((Component)__instance, source, destination); }
        catch (Exception e) { self.Fail(e); }
    }

    static void AfterPreCull(object __instance)
    {
        if (self == null || !self.enabledProbe.Value || self.nativeFailed) return;
        try { self.RequestCameraTextures((Component)__instance); }
        catch (Exception e) { self.Fail(e); }
    }

    void RequestCameraTextures(Component behaviour)
    {
        var candidate = behaviour.GetComponent<Camera>();
        if (!candidate || !candidate.GetComponent("GameCamera")) return;
        if (camera != candidate)
        {
            RestoreCamera();
            camera = candidate;
            addedFlags = (DepthTextureMode.Depth | DepthTextureMode.MotionVectors) & ~camera.depthTextureMode;
            Logger.LogInfo("historyResetRequired=CAMERA_CHANGED; history does not exist in diagnostics mode");
            Logger.LogInfo("cameraComponents=" + string.Join(",", camera.GetComponents<Component>().Where(x => x).Select(x => x.GetType().FullName)));
        }
        // PostProcessingBehaviour recalculates flags in OnPreCull. Request after it, before drawing.
        expectedFlags = camera.depthTextureMode | DepthTextureMode.Depth | DepthTextureMode.MotionVectors;
        camera.depthTextureMode = expectedFlags;
    }

    void Sample(Component behaviour, RenderTexture source, RenderTexture destination)
    {
        if (!enabledProbe.Value || pending || nativeFailed || Time.realtimeSinceStartup < nextSample) return;
        var candidate = behaviour.GetComponent<Camera>();
        // GameCamera is the gameplay camera owner. Camera.main can select a menu or mod camera.
        var owner = candidate ? candidate.GetComponent("GameCamera") : null;
        if (!owner || !source) return;
        if (camera != candidate) return;
        bool sizeChanged = width != source.width || height != source.height;
        bool jump = (camera.transform.position - previousPosition).sqrMagnitude > 100;
        if (sizeChanged || jump) Logger.LogInfo($"historyResetRequired sizeChanged={sizeChanged} cameraJumpOver10m={jump}");
        width = source.width; height = source.height; previousPosition = camera.transform.position;
        var depth = Shader.GetGlobalTexture("_CameraDepthTexture");
        var motion = Shader.GetGlobalTexture("_CameraMotionVectorsTexture");
        Logger.LogInfo($"camera={camera.name} source={Describe(source)} destination={(destination ? Describe(destination) : "BACKBUFFER")} screen={Screen.width}x{Screen.height} depthCandidate={Describe(depth)} motionCandidate={Describe(motion)} near={camera.nearClipPlane:R} far={camera.farClipPlane:R} fov={camera.fieldOfView:R} jitterM02={(camera.projectionMatrix.m02 - camera.nonJitteredProjectionMatrix.m02):R} jitterM12={(camera.projectionMatrix.m12 - camera.nonJitteredProjectionMatrix.m12):R} dtMs={Time.unscaledDeltaTime * 1000:F2} mapping=UNVERIFIED");
        nextSample = Time.realtimeSinceStartup + 3;
        if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Vulkan) { Logger.LogError("BLOCKED renderer is not Vulkan"); nativeFailed = true; return; }
        if (callback == IntPtr.Zero) callback = VF_GetEvent();
        if (callback == IntPtr.Zero) throw new InvalidOperationException("Native render callback is null");
        int eventId = VF_GetEventId();
        if (eventId < 0) throw new InvalidOperationException($"Native event unavailable; loadStatus={VF_GetLoadStatus()}");
        if (VF_Begin(source.GetNativeTexturePtr(), depth ? depth.GetNativeTexturePtr() : IntPtr.Zero,
            motion ? motion.GetNativeTexturePtr() : IntPtr.Zero) != 1) return;
        heldTextures = new Texture[] { source, depth, motion };
        pending = true; submittedAt = Time.realtimeSinceStartup;
        GL.IssuePluginEvent(callback, eventId);
        // Original postprocessing runs normally after this prefix. No Blit or dispatch is replaced.
    }

    static string Describe(Texture texture) => texture ? $"{texture.width}x{texture.height}/{texture.GetType().Name}" : "MISSING";
    void Fail(Exception error) { nativeFailed = true; Logger.LogError("BLOCKED diagnostic failure; ordinary rendering continues: " + error); }
    void RestoreCamera()
    {
        // Preserve changes made by other mods while the diagnostic was active.
        if (camera && camera.depthTextureMode == expectedFlags) camera.depthTextureMode &= ~addedFlags;
        camera = null;
    }
    void OnApplicationFocus(bool focus) { Logger.LogInfo($"focus={focus} historyResetRequired=true"); }
    void OnDestroy()
    {
        harmony?.UnpatchSelf(); RestoreCamera(); self = null;
        // Native slot has static lifetime. Do not unload a DLL while a render event may reference it.
        Logger.LogInfo($"diagnostics=STOPPED pendingRenderCallback={pending}; no NGX context was created");
    }
}
