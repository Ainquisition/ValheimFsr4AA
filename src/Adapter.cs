using System;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Bootstrap;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;
using UnityEngine.PostProcessing;

[BepInPlugin("local.valheim.fsr4aa.adapter", "Valheim OptiScaler AA Adapter", "0.2.2")]
public sealed class Adapter : BaseUnityPlugin
{
    const string Native = "ValheimFsr4AA.Native";
    [StructLayout(LayoutKind.Sequential)]
    struct Metadata { public float jitterX, jitterY, nearPlane, farPlane, fov, deltaMs; public uint reset; }
    [DllImport(Native, CallingConvention=CallingConvention.Cdecl)] static extern int VF_AdapterHealthy();
    [StructLayout(LayoutKind.Sequential)]
    struct FrameData { public IntPtr color, depth, motion, output; public Metadata camera; public uint normalized; }
    [DllImport(Native, CallingConvention=CallingConvention.Cdecl)] static extern int VF_FrameVersion();
    [DllImport(Native, CallingConvention=CallingConvention.Cdecl)] static extern IntPtr VF_FrameEvent();
    [DllImport(Native, CallingConvention=CallingConvention.Cdecl)] static extern int VF_FrameEventId(int slot);
    [DllImport(Native, CallingConvention=CallingConvention.Cdecl)] static extern int VF_FrameBegin(int slot, ref FrameData data);
    [DllImport(Native, CallingConvention=CallingConvention.Cdecl, CharSet=CharSet.Ansi)] static extern int VF_FrameRead(int slot, StringBuilder output, int capacity);
    static Adapter self;
    ConfigEntry<bool> active;
    Harmony harmony;
    const int Slots = 3;
    bool failed, reset = true;
    readonly bool[] pending = new bool[Slots];
    readonly float[] submittedAt = new float[Slots];
    readonly RenderTexture[][] bridgeImages = new RenderTexture[Slots][];
    readonly IntPtr[][] texturePointers = new IntPtr[Slots][];
    IntPtr callback;
    int nextSlot;
    Camera previousCamera;
    TaaComponent previousTaa;
    float previousFov, previousNear, previousFar;
    Vector3 previousPosition;
    float nextLog;
    int previousFrame = -2, frames;
    readonly StringBuilder report = new StringBuilder(4096);
    void Awake()
    {
        self = this;
        active = Config.Bind("Adapter", "Enabled", false, "Experimental Vulkan NGX adapter; F9 toggles. Requires clean pinned OptiScaler runtime and no competing scaler.");
        try
        {
            if (VF_FrameVersion() != 201 || Marshal.SizeOf(typeof(FrameData)) != 64) throw new InvalidOperationException("Native/managed frame ABI mismatch");
            callback = VF_FrameEvent();
        }
        catch (Exception e) { Block(e.ToString()); return; }
        harmony = new Harmony("local.valheim.fsr4aa.adapter");
        harmony.Patch(AccessTools.Method(typeof(TaaComponent), "Render"), prefix: new HarmonyMethod(typeof(Adapter), nameof(Render)));
        Logger.LogInfo("adapter=LOADED version=0.2.2 optiLoaded=" + (VF_AdapterHealthy() == 1) + " slots=3 coordinates=FSR_TOP_LEFT; F9 toggles; input=NOT_ATTEMPTED fsr4=NOT_VERIFIED");
    }
    bool CompetingScaler()
    {
        if (!Chainloader.PluginInfos.TryGetValue("com.wubarrk.graphicsoverdrive", out var info)) return false;
        var plugin = info.Instance as BaseUnityPlugin;
        if (plugin == null) return true;
        bool master = true, exclusive = false; string scaler = "Unknown";
        foreach (var entry in plugin.Config)
        {
            if (entry.Key.Key == "MasterEnable") master = (bool)entry.Value.BoxedValue;
            if (entry.Key.Key == "ExclusiveControl") exclusive = (bool)entry.Value.BoxedValue;
            if (entry.Key.Section == "Native Scaler" && entry.Key.Key == "Scaler") scaler = entry.Value.BoxedValue.ToString();
        }
        return master && (exclusive || scaler != "Vanilla");
    }
    void Update()
    {
        if (Input.GetKeyDown(KeyCode.F9)) { active.Value = !active.Value; reset = true; previousTaa?.ResetHistory(); Logger.LogInfo("adapterEnabled=" + active.Value); }
        Drain();
    }
    void Drain()
    {
        try
        {
            for (int slot = 0; slot < Slots; ++slot)
            {
            if (!pending[slot]) continue;
            report.Clear();
            if (VF_FrameRead(slot, report, report.Capacity) != 1)
            {
                if (Time.realtimeSinceStartup - submittedAt[slot] > 10) Block("render callback did not acknowledge; retained slot=" + slot);
                continue;
            }
            string text = report.ToString(); pending[slot] = false;
            if (!text.Contains("NGX_evaluate=1") || !text.Contains("fenceWait=0")) { Block(text); return; }
            ++frames;
            if (Time.realtimeSinceStartup >= nextLog)
            {
                Logger.LogInfo("evaluationsCompleted=" + frames + " " + text + " fsr4=CHECK_OPTISCALER_WATERMARK");
                nextLog = Time.realtimeSinceStartup + 3;
            }
            }
        }
        catch (Exception e) { Block(e.ToString()); }
    }
    static bool Render(TaaComponent __instance, RenderTexture source, RenderTexture destination)
    {
        if (self == null || !self.active.Value || self.failed) return true;
        try { return self.TryRender(__instance, source, destination); }
        catch (Exception e) { self.Block(e.ToString()); return true; }
    }
    bool TryRender(TaaComponent taa, RenderTexture source, RenderTexture destination)
    {
        Drain();
        if (failed) return true;
        var camera = taa.context.camera;
        if (!camera || !camera.GetComponent("GameCamera")) return true;
        if (CompetingScaler()) { Block("GraphicsOverdrive has ExclusiveControl or an active native scaler. Ordinary TAA continues."); return true; }
        if (SystemInfo.graphicsDeviceType != UnityEngine.Rendering.GraphicsDeviceType.Vulkan || !SystemInfo.usesReversedZBuffer || QualitySettings.activeColorSpace != ColorSpace.Linear)
        { Block("Requires Vulkan, reversed Z and linear color"); return true; }
        if (VF_AdapterHealthy() != 1) { Block("Pinned runtime not loaded from ValheimFsr4AA-runtime or explicit override, foreign dxgi.dll present, or GPU transaction failed"); return true; }
        var depth = Shader.GetGlobalTexture("_CameraDepthTexture");
        var motion = Shader.GetGlobalTexture("_CameraMotionVectorsTexture");
        if (!source || !destination || !depth || !motion || source.width != 2560 || source.height != 1440 ||
            destination.width != 2560 || destination.height != 1440 || depth.width != 2560 || depth.height != 1440 || motion.width != 2560 || motion.height != 1440)
        { reset = true; return true; }
        int slot = nextSlot;
        for (int i = 0; i < Slots && pending[slot]; ++i) slot = (slot + 1) % Slots;
        if (pending[slot]) { Block("FRAME_SLOTS_EXHAUSTED; effect stopped instead of alternating FSR/TAA"); return true; }
        reset |= camera != previousCamera || previousFrame != Time.frameCount - 1 || (camera.transform.position - previousPosition).sqrMagnitude > 100 ||
            previousFov != camera.fieldOfView || previousNear != camera.nearClipPlane || previousFar != camera.farClipPlane;
        if (reset) Logger.LogInfo("historyReset=1 camera=" + camera.name + " frame=" + Time.frameCount + " input=2560x1440 output=2560x1440");
        var jitter = taa.jitterVector;
        var cameraData = new Metadata { jitterX = -jitter.x * 2560, jitterY = jitter.y * 1440,
            nearPlane = camera.nearClipPlane, farPlane = camera.farClipPlane, fov = camera.fieldOfView * Mathf.Deg2Rad,
            deltaMs = Time.unscaledDeltaTime * 1000, reset = reset ? 1u : 0u };
        if (bridgeImages[slot] == null)
        {
            var formats = new[] {RenderTextureFormat.ARGBHalf, RenderTextureFormat.RFloat, RenderTextureFormat.RGHalf, RenderTextureFormat.ARGBHalf};
            bridgeImages[slot] = new RenderTexture[4];
            texturePointers[slot] = new IntPtr[4];
            for (int i = 0; i < 4; ++i)
            {
                var texture = new RenderTexture(2560, 1440, 0, formats[i], RenderTextureReadWrite.Linear);
                bridgeImages[slot][i] = texture;
                texture.name = "ValheimFsr4AA slot=" + slot + " texture=" + i;
                texture.filterMode = FilterMode.Point;
                if (!texture.Create()) throw new InvalidOperationException("Bridge RenderTexture.Create failed");
                texturePointers[slot][i] = texture.GetNativeTexturePtr();
            }
        }
        var images = bridgeImages[slot]; var pointers = texturePointers[slot];
        var flipScale = new Vector2(1,-1); var flipOffset = new Vector2(0,1);
        // Normalize all scene inputs to FSR coordinates, then convert the output back for Unity.
        Graphics.Blit(source, images[0], flipScale, flipOffset);
        Graphics.Blit(depth, images[1], flipScale, flipOffset);
        Graphics.Blit(motion, images[2], flipScale, flipOffset);
        Graphics.Blit(source, images[3], flipScale, flipOffset);
        var data = new FrameData {color=pointers[0], depth=pointers[1], motion=pointers[2], output=pointers[3], camera=cameraData, normalized=1};
        int eventId = VF_FrameEventId(slot);
        if (eventId < 0 || VF_FrameBegin(slot, ref data) != 1) { Block("Native frame slot was not free"); return true; }
        pending[slot] = true; submittedAt[slot] = Time.realtimeSinceStartup;
        GL.IssuePluginEvent(callback, eventId);
        Graphics.Blit(images[3], destination, flipScale, flipOffset);
        nextSlot = (slot + 1) % Slots;
        previousCamera = camera; previousPosition = camera.transform.position; previousFrame = Time.frameCount; reset = false;
        previousTaa = taa; previousFov = camera.fieldOfView; previousNear = camera.nearClipPlane; previousFar = camera.farClipPlane;
        return false;
    }
    void Block(string reason) { if (!failed) Logger.LogError("BLOCKED adapter: " + reason); failed = true; reset = true; previousTaa?.ResetHistory(); }
    void OnApplicationFocus(bool focus) { reset = true; }
    void OnDestroy()
    {
        harmony?.UnpatchSelf(); self = null;
        // Unity handles its later Blit work. Never explicitly destroy textures still owned by a native callback.
        int retained = 0;
        for (int slot = 0; slot < Slots; ++slot)
        {
            if (pending[slot]) { ++retained; continue; }
            if (bridgeImages[slot] != null) foreach (var texture in bridgeImages[slot]) if (texture) Destroy(texture);
        }
        Logger.LogInfo("adapter stopped; retainedPendingSlots=" + retained);
    }
}
