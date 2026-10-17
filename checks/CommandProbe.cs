// Test-only plugin. Skips the entry-point scene loader and never enters a world.
using System;
using System.Collections;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

[BepInPlugin("local.valheim.commandprobe", "Isolated Command Probe", "1.0.0")]
public sealed class CommandProbe : BaseUnityPlugin
{
    [StructLayout(LayoutKind.Sequential)] struct Metadata { public float jitterX,jitterY,nearPlane,farPlane,fov,deltaMs; public uint reset; }
    [StructLayout(LayoutKind.Sequential)] struct FrameData { public IntPtr color,depth,motion,output; public Metadata camera; public uint normalized; }
    [DllImport("ValheimFsr4AA.Native",CallingConvention=CallingConvention.Cdecl)] static extern int VF_FrameVersion();
    [DllImport("ValheimFsr4AA.Native",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr VF_FrameEvent();
    [DllImport("ValheimFsr4AA.Native",CallingConvention=CallingConvention.Cdecl)] static extern int VF_FrameEventId(int slot);
    [DllImport("ValheimFsr4AA.Native",CallingConvention=CallingConvention.Cdecl)] static extern int VF_FrameBegin(int slot,ref FrameData data);
    [DllImport("ValheimFsr4AA.Native",CallingConvention=CallingConvention.Cdecl,CharSet=CharSet.Ansi)] static extern int VF_FrameRead(int slot,StringBuilder output,int capacity);
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr VF_GetEvent();
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl)] static extern int VF_GetEventId();
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr VF_TestQueueEvent();
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl)] static extern void VF_TestNgxTextures(IntPtr color, IntPtr depth, IntPtr motion, IntPtr output);
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int VF_TestReadQueue(StringBuilder text, int capacity);
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl)] static extern int VF_TestGetEventId(int mode);
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl)] static extern int VF_Begin(IntPtr color, IntPtr depth, IntPtr motion);
    [DllImport("ValheimFsr4AA.Native", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int VF_Read(StringBuilder text, int capacity);
    string output;
    static bool Skip() => false;
    void Awake()
    {
        output = Environment.GetEnvironmentVariable("VF_COMMAND_RESULT");
        if (string.IsNullOrEmpty(output)) { enabled = false; return; }
        var entry = AccessTools.Method(AccessTools.TypeByName("EntryPointSceneLoader"), "Start");
        if (entry == null) throw new InvalidOperationException("EntryPointSceneLoader.Start missing");
        new Harmony("local.valheim.commandprobe").Patch(entry, prefix: new HarmonyMethod(typeof(CommandProbe), nameof(Skip)));
        Log("START renderer=" + SystemInfo.graphicsDeviceType + " threading=" + SystemInfo.renderingThreadingMode);
        StartCoroutine(Run());
    }
    void Log(string text) { File.AppendAllText(output, text + Environment.NewLine); Logger.LogInfo(text); }
    IEnumerator Run()
    {
        yield return null;
        var go = new GameObject("VF isolated offscreen camera");
        var camera = go.AddComponent<Camera>();
        camera.enabled = false; camera.cullingMask = 0; camera.clearFlags = CameraClearFlags.SolidColor;
        var texture = new RenderTexture(32, 32, 0, RenderTextureFormat.ARGBHalf);
        texture.Create(); camera.targetTexture = texture;
        var message = new StringBuilder(4096);
        bool ngx = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VF_OPTI_DLL")) || Environment.GetEnvironmentVariable("VF_QUEUE_ONLY") == "1";
        Log("LAUNCH optiOverridePresent=" + !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VF_OPTI_DLL")));
        RenderTexture depth = null, motion = null, result = null, consumed = null;
        if (ngx)
        {
            camera.targetTexture = null; Destroy(texture);
            texture = new RenderTexture(2560, 1440, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            depth = new RenderTexture(2560, 1440, 0, RenderTextureFormat.RFloat, RenderTextureReadWrite.Linear);
            motion = new RenderTexture(2560, 1440, 0, RenderTextureFormat.RGHalf, RenderTextureReadWrite.Linear);
            result = new RenderTexture(2560, 1440, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            consumed = new RenderTexture(2560, 1440, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            consumed.Create();
            var previous = RenderTexture.active;
            var textures = new[] {texture, depth, motion, result};
            var colors = new[] {new Color(0.25f, 0.5f, 0.75f, 1), new Color(0.5f, 0, 0, 0), Color.clear, Color.magenta};
            for (int i = 0; i < textures.Length; ++i)
            {
                textures[i].Create(); RenderTexture.active = textures[i]; GL.Clear(false, true, colors[i]);
            }
            RenderTexture.active = previous;
            camera.targetTexture = texture;
            VF_TestNgxTextures(texture.GetNativeTexturePtr(), depth.GetNativeTexturePtr(), motion.GetNativeTexturePtr(), result.GetNativeTexturePtr());
            Log("SYNTHETIC inputs=2560x1440 output=2560x1440 color=linear_RGBAHalf depth=RFloat_reversed motion=RGHalf_zero jitter=zero reset=1");
        }
        var names = new[] {"default", "sync", "flush", "sync_flush", "modifies"};
        var callback = VF_GetEvent();
        int locations = Environment.GetEnvironmentVariable("VF_QUEUE_ONLY") == "1" ? 0 : 3;
        for (int location = 0; location < locations; ++location)
        for (int mode = 0; mode < 5; ++mode)
        {
            int id = VF_TestGetEventId(mode);
            if (id < 0 || VF_Begin(texture.GetNativeTexturePtr(), IntPtr.Zero, IntPtr.Zero) != 1)
            { Log("FAIL event not ready"); Application.Quit(4); yield break; }
            CommandBuffer commands = null;
            CameraEvent point = location == 1 ? CameraEvent.BeforeImageEffects : CameraEvent.AfterEverything;
            if (location != 0)
            {
                commands = new CommandBuffer();
                commands.IssuePluginEvent(callback, id);
                camera.AddCommandBuffer(point, commands);
            }
            camera.Render();
            if (location == 0) GL.IssuePluginEvent(callback, id);
            float deadline = Time.realtimeSinceStartup + 5;
            bool received = false;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                message.Clear();
                if (VF_Read(message, message.Capacity) == 1) { received = true; break; }
            }
            Log("CASE location=" + (location == 0 ? "GL" : point.ToString()) + " mode=" + names[mode] +
                " received=" + received + " " + message);
            if (commands != null) { camera.RemoveCommandBuffer(point, commands); commands.Release(); }
            if (!received) { Application.Quit(5); yield break; }
        }
        for (int frame = 0; frame < (ngx ? 3 : 1); ++frame)
        {
        var expectedColor = new Color(0.25f + 0.1f * frame, 0.5f, 0.75f - 0.1f * frame, 1);
        if (ngx)
        {
            var previous = RenderTexture.active;
            RenderTexture.active = texture; GL.Clear(false, true, expectedColor);
            RenderTexture.active = result; GL.Clear(false, true, Color.magenta);
            RenderTexture.active = previous;
        }
        GL.IssuePluginEvent(VF_TestQueueEvent(), VF_GetEventId());
        if (ngx) Graphics.Blit(result, consumed); // Immediate Unity consumer, exactly as in the adapter.
        float queueDeadline = Time.realtimeSinceStartup + 5;
        bool queueReceived = false;
        while (Time.realtimeSinceStartup < queueDeadline)
        {
            yield return null;
            message.Clear();
            if (VF_TestReadQueue(message, message.Capacity) == 1) { queueReceived = true; break; }
        }
        Log("QUEUE received=" + queueReceived + " " + message);
        if (ngx && queueReceived)
        {
            // Test-only readback after the fence; the game adapter has no readback path.
            var previous = RenderTexture.active;
            RenderTexture.active = consumed;
            var image = new Texture2D(2560, 1440, TextureFormat.RGBA32, false, true);
            image.ReadPixels(new Rect(0, 0, 2560, 1440), 0, 0); image.Apply();
            RenderTexture.active = previous;
            var center = image.GetPixel(1280, 720);
            Log("UNITY_CONSUMER frame=" + frame + " center=" + center + " expected=" + expectedColor + " nativeOutputInitialSentinel=magenta");
            File.WriteAllBytes(output + ".png", image.EncodeToPNG());
            Destroy(image);
            if (!message.ToString().Contains("NGX_evaluate=1") || !message.ToString().Contains("fenceWait=0") ||
                Mathf.Abs(center.r - expectedColor.r) > 0.02f || Mathf.Abs(center.g - expectedColor.g) > 0.02f || Mathf.Abs(center.b - expectedColor.b) > 0.02f)
            { Log("FAIL dispatch/fence/output RGB"); Application.Quit(7); yield break; }
        }
        if (!queueReceived) { Application.Quit(6); yield break; }
        }
        if (ngx)
        {
            // Asymmetric static pattern: compare orientation against an ordinary Unity Blit, independent of PNG row order.
            var pattern = new Texture2D(2560, 1440, TextureFormat.RGBA32, false, true);
            var pixels = new Color32[2560 * 1440];
            var colors = new[] {new Color32(200,30,40,255), new Color32(30,200,50,255), new Color32(40,50,200,255), new Color32(200,180,40,255)};
            for (int y = 0; y < 1440; ++y)
            for (int x = 0; x < 2560; ++x) pixels[y * 2560 + x] = colors[(x < 1280 ? 0 : 1) + (y < 720 ? 0 : 2)];
            pattern.SetPixels32(pixels); pattern.Apply();
            var reference = new RenderTexture(2560, 1440, 0, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
            reference.Create();
            Graphics.Blit(pattern, texture); Graphics.Blit(texture, reference);
            var normalizedColor = new RenderTexture(2560,1440,0,RenderTextureFormat.ARGBHalf,RenderTextureReadWrite.Linear);
            normalizedColor.Create();
            var flipScale = new Vector2(1,-1); var flipOffset = new Vector2(0,1);
            Graphics.Blit(texture,normalizedColor,flipScale,flipOffset);
            var data = new FrameData {color=normalizedColor.GetNativeTexturePtr(),depth=depth.GetNativeTexturePtr(),motion=motion.GetNativeTexturePtr(),output=result.GetNativeTexturePtr(),
                camera=new Metadata {nearPlane=0.1f,farPlane=1000,fov=1.04719755f,deltaMs=16.67f,reset=1},normalized=1};
            if (Marshal.SizeOf(typeof(FrameData)) != 64 || VF_FrameVersion()!=201 || VF_FrameBegin(0,ref data)!=1) { Application.Quit(10); yield break; }
            GL.IssuePluginEvent(VF_FrameEvent(),VF_FrameEventId(0)); Graphics.Blit(result, consumed,flipScale,flipOffset);
            bool received = false;
            float deadline = Time.realtimeSinceStartup + 5;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null; message.Clear();
                if (VF_FrameRead(0,message,message.Capacity) == 1) { received = true; break; }
            }
            Log("ORIENTATION dispatch=" + received + " " + message);
            if (!received) { Application.Quit(8); yield break; }
            var previous = RenderTexture.active;
            var images = new[] {new Texture2D(2560,1440,TextureFormat.RGBA32,false,true), new Texture2D(2560,1440,TextureFormat.RGBA32,false,true)};
            for (int i = 0; i < 2; ++i)
            {
                RenderTexture.active = i == 0 ? reference : consumed;
                images[i].ReadPixels(new Rect(0,0,2560,1440),0,0); images[i].Apply();
            }
            RenderTexture.active = previous;
            foreach (int y in new[] {360,1080}) foreach (int x in new[] {512,2048})
            {
                var expected = images[0].GetPixel(x,y); var actual = images[1].GetPixel(x,y);
                Log("ORIENTATION sample=" + x + "," + y + " reference=" + expected + " FSR=" + actual);
                if (Mathf.Abs(expected.r-actual.r)>0.02f || Mathf.Abs(expected.g-actual.g)>0.02f || Mathf.Abs(expected.b-actual.b)>0.02f)
                { Log("FAIL spatial orientation/RGB"); Application.Quit(9); yield break; }
            }
            File.WriteAllBytes(output + ".orientation.png",images[1].EncodeToPNG());
            File.WriteAllBytes(output + ".reference.png",images[0].EncodeToPNG());
            foreach (var image in images) Destroy(image);
            Destroy(pattern); Destroy(reference); Destroy(normalizedColor);
            // Three queued frames must retain separate image pointers and metadata before any acknowledgement is read.
            var ring = new RenderTexture[3][];
            for (int slot=0;slot<3;++slot)
            {
                ring[slot]=new RenderTexture[5];
                var formats=new[] {RenderTextureFormat.ARGBHalf,RenderTextureFormat.RFloat,RenderTextureFormat.RGHalf,RenderTextureFormat.ARGBHalf,RenderTextureFormat.ARGBHalf};
                for (int i=0;i<5;++i) { ring[slot][i]=new RenderTexture(2560,1440,0,formats[i],RenderTextureReadWrite.Linear); ring[slot][i].Create(); }
            }
            for (int batch=0;batch<3;++batch)
            {
                for (int slot=0;slot<3;++slot)
                {
                    int sampleSlot=batch==2 ? 2 : slot;
                    var expected=new Color(0.2f+sampleSlot*0.2f,0.4f,0.7f-sampleSlot*0.1f,1);
                    var before=RenderTexture.active;
                    for (int i=0;i<4;++i) { RenderTexture.active=ring[slot][i]; GL.Clear(false,true,i==0 ? expected : i==1 ? new Color(0.5f,0,0,0) : i==2 ? Color.clear : Color.magenta); }
                    RenderTexture.active=before;
                    data.color=ring[slot][0].GetNativeTexturePtr(); data.depth=ring[slot][1].GetNativeTexturePtr(); data.motion=ring[slot][2].GetNativeTexturePtr(); data.output=ring[slot][3].GetNativeTexturePtr();
                    data.camera.reset=batch==2 ? 0u : 1u;
                    if (VF_FrameBegin(slot,ref data)!=1 || VF_FrameBegin(slot,ref data)!=0) { Log("FAIL native slot ownership"); Application.Quit(11); yield break; }
                    GL.IssuePluginEvent(VF_FrameEvent(),VF_FrameEventId(slot)); Graphics.Blit(ring[slot][3],ring[slot][4],flipScale,flipOffset);
                }
                for (int slot=0;slot<3;++slot)
                {
                    bool done=false; float until=Time.realtimeSinceStartup+5;
                    while (Time.realtimeSinceStartup<until) { yield return null; message.Clear(); if(VF_FrameRead(slot,message,message.Capacity)==1){done=true;break;} }
                    Log("RING batch="+batch+" slot="+slot+" received="+done+" "+message);
                    if (!done || !message.ToString().Contains("fenceWait=0")) { Application.Quit(12); yield break; }
                    var before=RenderTexture.active; RenderTexture.active=ring[slot][4];
                    var image=new Texture2D(2560,1440,TextureFormat.RGBA32,false,true);
                    image.ReadPixels(new Rect(0,0,2560,1440),0,0); image.Apply(); RenderTexture.active=before;
                    int sampleSlot=batch==2 ? 2 : slot;
                    var actual=image.GetPixel(1280,720); var expected=new Color(0.2f+sampleSlot*0.2f,0.4f,0.7f-sampleSlot*0.1f,1);
                    Log("RING pixel="+actual+" expected="+expected);
                    Destroy(image);
                    if (Mathf.Abs(actual.r-expected.r)>0.02f || Mathf.Abs(actual.g-expected.g)>0.02f || Mathf.Abs(actual.b-expected.b)>0.02f) { Application.Quit(13); yield break; }
                }
            }
            foreach(var set in ring) foreach(var image in set) Destroy(image);
        }
        Log("DONE eventCases=" + locations * 5 + "; entry scene only; NGX test=" + ngx);
        camera.targetTexture = null; Destroy(texture); Destroy(go);
        if (ngx) { Destroy(depth); Destroy(motion); Destroy(result); Destroy(consumed); }
        Application.Quit(0);
    }
}
