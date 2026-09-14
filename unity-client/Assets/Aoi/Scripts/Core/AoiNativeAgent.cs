using System;
using System.Runtime.InteropServices;
using System.Threading;
using UnityEngine;

// Bridges the native C++ Aoi agent (aoi_agent.dll) into Unity via P/Invoke.
//
// The agent ALWAYS runs on its OWN background thread inside the DLL (never the
// Unity main thread), so the render loop is never blocked.
//
// Messaging is IN-PROCESS — there is no named pipe:
//   - agent -> Unity: AoiAgent_SetMessageCallback registers a static callback;
//     the agent's background thread invokes it with each outbound JSON message.
//     We marshal onto the Unity main thread and hand it to AoiOrchestrator.
//   - Unity -> agent: AoiAgent_SendJson pushes an inbound message into the agent.
//
// Usage: attach to the same GameObject as AoiOrchestrator. autoStart handles
// the whole lifecycle.
public class AoiNativeAgent : MonoBehaviour
{
    [Header("Runtime")]
    [Tooltip("Working directory containing aoi_config.json (relative to the build folder).")]
    public string workDir = "";
    [Tooltip("Automatically start the agent on Awake.")]
    public bool autoStart = true;

    private Thread agentThread;   // managed thread from which the DLL is driven
    private bool stopping;
    private AoiOrchestrator orchestrator;

    // Serializes Start/Shutdown so the driver thread and a stop thread never
    // race into AoiAgent_Start/AoiAgent_Stop concurrently (the DLL now also
    // holds a lifecycle mutex, but the C# side stays clean too).
    private readonly object lifecycleLock = new object();

    // Outbound JSON messages from the agent, marshaled to the main thread.
    private readonly System.Collections.Concurrent.ConcurrentQueue<string> agentOutbox =
        new System.Collections.Concurrent.ConcurrentQueue<string>();

    // NOTE: all native strings are UTF-8 (the C++ side uses std::string / JSON
    // which are UTF-8). We use UnmanagedType.LPUTF8Str (Unity .NET Standard 2.1)
    // so Chinese never goes through the system ANSI/GBK code page. Callback
    // pointers are decoded with Marshal.PtrToStringUTF8.
    // IL2CPP resolves P/Invoke BY NAME; an external build layer may rewrite
    // the EntryPoint below for shipped builds (the readable names here are the
    // canonical ABI).
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SetEnv", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AoiAgent_SetEnv([MarshalAs(UnmanagedType.LPUTF8Str)] string name,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_Start", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_Start();
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_Stop", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AoiAgent_Stop();
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_IsRunning", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_IsRunning();
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SetMessageCallback", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AoiAgent_SetMessageCallback(AoiAgent_MessageCallback cb);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SetLogCallback", CallingConvention = CallingConvention.Cdecl)]
    private static extern void AoiAgent_SetLogCallback(AoiAgent_LogCallback cb);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendJson", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendJson([MarshalAs(UnmanagedType.LPUTF8Str)] string json);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendStateChange", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendStateChange([MarshalAs(UnmanagedType.LPUTF8Str)] string state,
                                                       [MarshalAs(UnmanagedType.LPUTF8Str)] string mode,
                                                       [MarshalAs(UnmanagedType.LPUTF8Str)] string shotPath);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendTtsStop", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendTtsStop();
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendScreenshotPath", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendScreenshotPath([MarshalAs(UnmanagedType.LPUTF8Str)] string requestId,
                                                          [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendScreenshotImage", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendScreenshotImage([MarshalAs(UnmanagedType.LPUTF8Str)] string requestId,
                                                           [MarshalAs(UnmanagedType.LPUTF8Str)] string base64Jpeg);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendScreenshotError", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendScreenshotError([MarshalAs(UnmanagedType.LPUTF8Str)] string requestId,
                                                           [MarshalAs(UnmanagedType.LPUTF8Str)] string error);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendVrSkillResult", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendVrSkillResult([MarshalAs(UnmanagedType.LPUTF8Str)] string requestId,
                                                         [MarshalAs(UnmanagedType.LPUTF8Str)] string resultJson);
    [DllImport("aoi_agent.dll", EntryPoint = "AoiAgent_SendDisplayResult", CallingConvention = CallingConvention.Cdecl)]
    private static extern int AoiAgent_SendDisplayResult(bool success);

    // IL2CPP requires a STATIC method with MonoPInvokeCallback for native
    // callbacks. We stash the instance and enqueue into its outbox.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void AoiAgent_MessageCallback(IntPtr jsonPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void AoiAgent_LogCallback(IntPtr linePtr);

    private static AoiNativeAgent s_instance;

    [AOT.MonoPInvokeCallback(typeof(AoiAgent_MessageCallback))]
    private static void OnAgentMessage(IntPtr jsonPtr)
    {
        // Runs on the agent's background thread. Copy the string and enqueue.
        var inst = s_instance;
        if (inst == null) return;
        string json = Marshal.PtrToStringUTF8(jsonPtr);
        if (json == null) return;
        inst.agentOutbox.Enqueue(json);
    }

    [AOT.MonoPInvokeCallback(typeof(AoiAgent_LogCallback))]
    private static void OnAgentLog(IntPtr linePtr)
    {
        // Agent internal logs -> Unity. Runs on the agent's thread; Debug.Log
        // is thread-safe enough for diagnostics.
        string line = Marshal.PtrToStringUTF8(linePtr);
        if (!string.IsNullOrEmpty(line))
            Debug.Log("[Agent] " + line);
    }

    void Awake()
    {
        s_instance = this;
        if (AoiOrchestrator.QuittingDueToSecondInstance)
        {
            enabled = false;
            return;
        }
        if (!autoStart) return;
        StartCoroutine(WaitForOrchestratorThenStart());
    }

    System.Collections.IEnumerator WaitForOrchestratorThenStart()
    {
        for (int i = 0; i < 60; i++)
        {
            orchestrator = FindFirstObjectByType<AoiOrchestrator>();
            if (orchestrator != null) break;
            yield return new UnityEngine.WaitForSeconds(0.5f);
        }
        InitAndStart();
    }

    public void InitAndStart()
    {
        if (AoiOrchestrator.QuittingDueToSecondInstance) return;
        // The native agent reads ALL LLM/TTS settings from aoi_config.json in
        // the working directory. We only resolve where that directory is.
        if (string.IsNullOrEmpty(workDir))
            workDir = FirstNonEmpty(GetEnv("AOI_CWD"),
                                    System.IO.Path.GetFullPath(System.IO.Path.Combine(Application.dataPath, "..")));
        if (!string.IsNullOrEmpty(workDir))
            SetEnvIfNotBlank("AOI_CWD", workDir);

        if (AoiAgent_IsRunning() == 1)
        {
            Debug.Log("[AoiNativeAgent] already running");
            return;
        }

        lock (lifecycleLock)
        {
            // Double-check under the lock (a Shutdown could have completed
            // between the check above and acquiring the lock).
            if (stopping) return;
            if (AoiAgent_IsRunning() == 1)
            {
                Debug.Log("[AoiNativeAgent] already running");
                return;
            }

            // Register the outbound callback (static, MonoPInvokeCallback for IL2CPP).
            AoiAgent_SetMessageCallback(OnAgentMessage);
            // Register the log callback so the agent's internal logs appear in Unity.
            AoiAgent_SetLogCallback(OnAgentLog);

            // Drive the DLL from a dedicated managed background thread. The DLL
            // itself also runs the agent on its own child thread, so even this
            // managed thread never blocks the main one.
            stopping = false;
            agentThread = new Thread(() =>
            {
                int rc = AoiAgent_Start();
                Debug.Log($"[AoiNativeAgent] Start rc={rc} (workdir={workDir})");
                while (!stopping && AoiAgent_IsRunning() == 1)
                    Thread.Sleep(200);
            })
            { IsBackground = true, Name = "AoiNativeAgentDriver" };
            agentThread.Start();
        }
    }

    // ---- config helpers ----
    static string GetEnv(string name)
    {
        try { return Environment.GetEnvironmentVariable(name) ?? ""; }
        catch { return ""; }
    }

    static string FirstNonEmpty(params string[] values)
    {
        foreach (var v in values)
            if (!string.IsNullOrEmpty(v)) return v;
        return "";
    }

    void Update()
    {
        // Drain the agent's outbox on the main thread, feeding AoiOrchestrator.
        if (orchestrator == null)
            orchestrator = FindFirstObjectByType<AoiOrchestrator>();
        while (agentOutbox.TryDequeue(out var json))
        {
            if (orchestrator != null)
                orchestrator.HandleAgentMessage(json);
        }
    }

    // Unity -> agent (raw JSON, kept for debugging; prefer the semantic
    // SendStateChange/SendScreenshot*/... wrappers below).
    public bool SendToAgent(string json)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendJson(json) == 1;
    }

    // ---- Semantic senders: protocol/schema lives in the DLL, not here. ----
    public bool SendStateChange(string state, string mode, string shotPath)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendStateChange(state ?? "", mode ?? "", shotPath ?? "") == 1;
    }

    public bool SendTtsStop()
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendTtsStop() == 1;
    }

    public bool SendScreenshotPath(string requestId, string path)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendScreenshotPath(requestId ?? "", path ?? "") == 1;
    }

    public bool SendScreenshotImage(string requestId, string base64Jpeg)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendScreenshotImage(requestId ?? "", base64Jpeg ?? "") == 1;
    }

    public bool SendScreenshotError(string requestId, string error)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendScreenshotError(requestId ?? "", error ?? "") == 1;
    }

    public bool SendVrSkillResult(string requestId, string resultJson)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendVrSkillResult(requestId ?? "", resultJson ?? "{}") == 1;
    }

    public bool SendDisplayResult(bool success)
    {
        if (AoiAgent_IsRunning() != 1) return false;
        return AoiAgent_SendDisplayResult(success) == 1;
    }

    void OnApplicationQuit()
    {
        Shutdown();
    }

    public void Shutdown()
    {
        lock (lifecycleLock)
        {
            if (stopping) return;  // idempotent (OnDestroy + OnApplicationQuit)
            stopping = true;
            AoiAgent_SetMessageCallback(null);
            AoiAgent_SetLogCallback(null);
            // Stop the DLL on its own driver thread so the main thread is never
            // blocked waiting on in-flight LLM/TTS HTTP.
            var t = agentThread;
            agentThread = null;
            if (t != null && t.IsAlive)
            {
                Thread stopThread = new Thread(() => AoiAgent_Stop())
                { IsBackground = true, Name = "AoiAgentStop" };
                stopThread.Start();
                if (!stopThread.Join(5000)) stopThread = null;  // give up, don't hang shutdown
                t.Join(5000);
            }
            else
            {
                AoiAgent_Stop();
            }
            if (s_instance == this) s_instance = null;
            Debug.Log("[AoiNativeAgent] stopped");
        }
    }

    void OnDestroy()
    {
        Shutdown();
    }

    static void SetEnvIfNotBlank(string name, string value)
    {
        if (string.IsNullOrEmpty(value)) return;
        try { Environment.SetEnvironmentVariable(name, value); }
        catch (Exception e) { Debug.LogWarning($"[AoiNativeAgent] SetEnvironmentVariable({name}) failed: {e.Message}"); }
    }
}
