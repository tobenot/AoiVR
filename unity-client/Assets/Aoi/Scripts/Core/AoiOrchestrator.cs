
using UnityEngine;

using UnityEngine.UI;

using TMPro;

using Valve.VR;

using System;

using System.Collections.Concurrent;

using System.Runtime.InteropServices;

using System.Text;

using System.Collections;

using System.Collections.Generic;
public class AoiOrchestrator : MonoBehaviour{
    [Header("Overlay Settings")]    public string overlayKey = "aoi.hand.panel";
    public string overlayName = "Aoi Hand Panel";
    public float overlayWidthMeters = 0.3f;
    public float handOffsetForward = 0.18f;
    public float handOffsetDown = 0.05f;
    public float handOffsetUp = 0.05f;
    public float handOffsetRight = 0.0f;
    public float panelAutoHideSeconds = 0f;
    public float overlayAlpha = 0.85f;
    [Header("Render")]    public int textureWidth = 1024;
    public int textureHeight = 1024;
    public Camera uiCamera;
    public RenderTexture renderTexture;
    public enum State {
 Standby, Active, Error }
    public State CurrentState {
 get;
 private set;
 }
 = State.Standby;
    [HideInInspector] public ulong handOverlayHandle = 0;
    [HideInInspector] public ulong dashboardOverlayHandle = 0;
    [HideInInspector] public ulong dashboardThumbHandle = 0;
    public static TMP_FontAsset uiFont;
    public static TMP_FontAsset monoFont;
    public static bool DesktopMode;
    public static bool DemoMode;
    public static bool DemoRecordActive;
    public static bool QuittingDueToSecondInstance { get; private set; }
    string demoRecDir;
    float demoRecStart;
    float demoRecTimer;
    float demoRecLast;
    int demoRecFrames;
    // True when the ScreenMirror camera exists (desktop window shows the
    // panel). The panel render texture is then rendered continuously and the
    // desktop window accepts mouse input, like DesktopMode.
    public static bool WindowMirrorActive;
    // Panel render-texture size (window mouse coords are scaled into this
    // space for the WorldSpace canvas raycast).
    public static int PanelTexWidth = 1024;
    public static int PanelTexHeight = 1024;
    public static TMP_FontAsset ResolveUIFont()    {
        if (uiFont != null) return uiFont;
        uiFont = Resources.Load<TMP_FontAsset>("Fonts & Materials/LiberationSans SDF");
        if (uiFont == null)            uiFont = Resources.Load<TMP_FontAsset>("AoiSDF");
        return uiFont;
    }
    // Monospace (JetBrains Mono) font for terminal-style texts; falls back to the UI font.
    public static TMP_FontAsset ResolveMonoFont()    {
        if (monoFont != null) return monoFont;
        monoFont = Resources.Load<TMP_FontAsset>("AoiMono");
        if (monoFont == null)            monoFont = ResolveUIFont();
        return monoFont;
    }
    private bool openVRReady = false;
    private bool gripWasPressed = false;
    private bool calling = false;
    private bool steamVRRunning = false;
    private bool overlayVisible = false;
    private bool dashWasVisible = false;
    private bool textureSubmitted = false;
    private System.IO.StreamWriter logWriter;
    private SteamVROverlayInput inputComponent;
    // System.Threading.Mutex is not supported by the Windows IL2CPP player in
    // this Unity version. Use the Win32 named mutex directly so the single-
    // instance guard is functional in the shipped desktop build.
    private IntPtr singleInstanceMutexHandle = IntPtr.Zero;
    private bool quittingDueToSecondInstance = false;
    private AudioSource clickAudio;
    private AudioClip clickClip;
    private HandPanelUI cachedPanelUI;
    private bool shutdownDone = false;
    private VRActiveActionSet_t[] gripActionSets = new VRActiveActionSet_t[1];
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")] private static extern IntPtr GetTopWindow(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool AllocConsole();
    [DllImport("kernel32.dll", EntryPoint = "CreateMutexW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateMutexW(IntPtr lpMutexAttributes, bool bInitialOwner, string lpName);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr hObject);
    [DllImport("kernel32.dll")] private static extern IntPtr GetStdHandle(int nStdHandle);
    private const int STD_OUTPUT_HANDLE = -11;
    private const int ERROR_ALREADY_EXISTS = 183;
    private static readonly IntPtr HWND_BOTTOM = new IntPtr(1);
    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOACTIVATE = 0x0010;
    private ConcurrentQueue<Action> mainThreadActions = new ConcurrentQueue<Action>();
    private TrackedDevicePose_t[] laserPoseBuffer;
    private ulong laserOverlayHandle = 0;
    private const float laserThicknessMeters = 0.002f;
    private bool laserVisible = false;
    private ulong triggerActionHandle = 0;
    private bool triggerWasPressed = false;
    private VROverlayIntersectionResults_t lastLaserHit;
    private bool laserHitThisFrame = false;
    private readonly object logLock = new object();
    void Log(string msg)    {
        var line = $"[{DateTime.Now:HH:mm:ss}] {msg}";
        lock (logLock)        {
            try { Debug.Log(line); } catch { }
            try {
  logWriter?.WriteLine(line);
  logWriter?.Flush();
  }
  catch {
  }
        }
    }
    void Awake()    {
        DontDestroyOnLoad(gameObject);
        QuittingDueToSecondInstance = false;
        try        {
            singleInstanceMutexHandle = CreateMutexW(IntPtr.Zero, true, @"Global\AoiVR_SingleInstance");
            int mutexError = Marshal.GetLastWin32Error();
            if (singleInstanceMutexHandle != IntPtr.Zero && mutexError == ERROR_ALREADY_EXISTS)            {
                QuittingDueToSecondInstance = true;
                quittingDueToSecondInstance = true;
                Debug.Log("Another AoiVR instance is already running.");
                AllocConsole();
                var h = GetStdHandle(STD_OUTPUT_HANDLE);
                if (h != IntPtr.Zero)                {
                    var sw = new System.IO.StreamWriter(                        new System.IO.FileStream(                            new Microsoft.Win32.SafeHandles.SafeFileHandle(h, false),                            System.IO.FileAccess.Write), Encoding.UTF8);
                    sw.AutoFlush = true;
                    sw.WriteLine("Another AoiVR instance is already running.");
                    sw.WriteLine("This instance will exit in 3 seconds.");
                }
                CloseHandle(singleInstanceMutexHandle);
                singleInstanceMutexHandle = IntPtr.Zero;
                System.Threading.Thread.Sleep(3000);
                Application.Quit();
                return;
            }
            if (singleInstanceMutexHandle == IntPtr.Zero)            {
                Debug.Log($"Mutex check failed (CreateMutexW win32={mutexError}, continuing)");
            }
        }
        catch (System.Exception e)        {
            Debug.Log($"Mutex check failed (continuing): {e.Message}");
        }
        logWriter = new System.IO.StreamWriter(Application.dataPath + "/../aoi_debug.txt", false);
        Log("Awake");
    }
    void Start()    {
        // Never run the scene when this instance is just about to quit (a second
        // instance won the mutex). Awake's Application.Quit() doesn't prevent
        // Start from still firing.
        if (quittingDueToSecondInstance) return;
        DemoMode = System.Environment.CommandLine.Contains("-demo");
        DesktopMode = System.Environment.CommandLine.Contains("-desktop");
        // Always start windowed: a fullscreen player window left users with a
        // borderless full-screen window and no visible close button.
        Screen.fullScreen = false;
        Application.targetFrameRate = 30;
        Application.runInBackground = true;
        QualitySettings.vSyncCount = 0;
        var font = ResolveUIFont();
        Log(font != null ? $"UI font resolved: {font.name}" : "UI font RESOLUTION FAILED (null)");
        foreach (var s in new[] {
 "UI/Default", "Sprites/Default", "Unlit/Color", "TextMeshPro/Distance Field", "TextMeshPro/Distance Field Overlay" }
)            Log($"Shader '{s}': {(Shader.Find(s) != null ? "found" : "MISSING (stripped)")}");
        if (DemoMode)        {
            Log("Start - DEMO MODE");
            StartDesktopMode();            // windowed + canvas overlay
            DemoRecordActive = System.Environment.CommandLine.Contains("-demo-record");
            StartCoroutine(DemoModeRoutine());
            return;
        }
        if (DesktopMode)        {
            Log("Start - DESKTOP MODE");
            StartDesktopMode();
            return;
        }
        Log("Start - hiding window");
        HideWindow();
        StartCoroutine(WaitForSteamVR());
    }
    void StartDesktopMode()    {
        // The panel is designed on a 684x684 canvas; keep the window square so
        // the layout isn't stretched. (Previously this forced 1280x800 wide.)
        Screen.SetResolution(684, 684, false);
        var camGO = new GameObject("DesktopCamera");
        var cam = camGO.AddComponent<Camera>();
        cam.clearFlags = CameraClearFlags.SolidColor;
        cam.backgroundColor = new Color(0.02f, 0.04f, 0.06f);
        cam.orthographic = true;
        cam.cullingMask = 0;
        cam.depth = -10;
        var canvas = GameObject.Find("HandPanelCanvas")?.GetComponent<Canvas>();
        if (canvas != null)        {
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            var crt = canvas.GetComponent<RectTransform>();
            crt.localScale = Vector3.one;
            Log("Canvas set to ScreenSpaceOverlay for desktop");
        }
        UpdateInputComponent();
        StartCoroutine(CaptureScreenDiag());
        SetPanelStatus("● 就绪");
        UpdateHint();
        Log("Desktop mode initialized");
    }
    System.Collections.IEnumerator CaptureScreenDiag()    {
        yield return null;
        yield return new WaitForEndOfFrame();
        yield return null;
        yield return new WaitForEndOfFrame();
        LogTextDiagnostics();
        ScreenCapture.CaptureScreenshot(Application.dataPath + "/../desktop_capture.png");
        Log("Desktop screenshot captured to desktop_capture.png");
    }

    // Demo mode (-desktop -demo): drives the panel UI with scripted, front-end
    // only scenes (no agent, no LLM/TTS/audio). Highlights the product's
    // capabilities for screenshots / recordings: environment context,
    // screenshot, chat, VR brightness, system volume.
    // -demo-record additionally captures frames via ScreenCapture (the app's
    // OWN render output — never the desktop) into demo_rec/frame_%04d.png and
    // exits after ~30s, for building clean demo GIFs.
    System.Collections.IEnumerator DemoModeRoutine()    {
        yield return null; // let the desktop UI initialize
        var ui = GetPanelUI();
        if (ui == null)        {
            Log("[Demo] no panel UI found");
            yield break;
        }
        Log("[Demo] running");
        while (true)        {
            ui.SetStatus("● 就绪");
            ui.SetHint("<color=#02D7F2>Demo 模式</color> — 模拟对话演示");
            yield return new WaitForSeconds(1.2f);

            // Scene 1: environment awareness
            yield return DemoScene(ui, "我现在在看什么？", "正在获取环境上下文…",
                "你正站在 SteamVR 主菜单里，面前是应用库和商店入口。环境感知已开启，我可以持续看到你周围的画面并听到系统声音。",
                u => { u.SetEnvActive(true); u.ShowShotToast("环境感知已开启"); });
            yield return new WaitForSeconds(1.5f);

            // Scene 2: screenshot
            yield return DemoScene(ui, "帮我截个图", "正在截取当前画面…",
                "已经帮你截好了，画面已保存。你可以随时让我截图或回顾刚才的内容。",
                u => u.ShowShotToast("已截图"));
            yield return new WaitForSeconds(1.5f);

            // Scene 3: chat
            yield return DemoScene(ui, "帮我查一下明天的天气", "正在思考…",
                "根据环境信息，明天多云转晴，气温 18~26°C，适合出门。需要我帮你安排什么吗？",
                null);
            yield return new WaitForSeconds(1.5f);

            // Scene 4: VR brightness
            yield return DemoScene(ui, "太亮了，调暗一点", "正在调整 VR 亮度…",
                "好的，已把 VR 亮度调暗至 70%。如果还想更暗随时告诉我。",
                u => u.SetHint("<color=#F3E600>VR 亮度 70%</color>"));
            yield return new WaitForSeconds(1.5f);

            // Scene 5: system volume
            yield return DemoScene(ui, "音量调低一点", "正在调整系统音量…",
                "系统音量已调到 40%，现在的声音应该舒服多了。",
                u => u.SetHint("<color=#F3E600>系统音量 40%</color>"));
            yield return new WaitForSeconds(3f);
        }
    }

    System.Collections.IEnumerator DemoScene(HandPanelUI ui, string userText, string thought,
                                             string reply, System.Action<HandPanelUI> extra)    {
        ui.SetStatus("● 思考中…", 1);
        ui.ShowProcessing(true);
        ui.SetProcessingStage("thinking", thought);
        yield return new WaitForSeconds(1.1f);
        ui.AppendUserText(userText);
        ui.SetProcessingStage("generate", null);
        // Typewriter effect.
        for (int i = 1; i <= reply.Length; i += 3)        {
            ui.AppendChatText(reply.Substring(0, System.Math.Min(i, reply.Length)), false);
            yield return new WaitForSeconds(0.02f);
        }
        ui.AppendChatText(reply, true);
        ui.ResetProcessing();
        ui.ShowProcessing(false);
        ui.SetStatus("● 就绪");
        if (extra != null) extra(ui);
    }
    System.Collections.IEnumerator WaitForSteamVR()    {
        steamVRRunning = false;
        Log("Waiting for SteamVR (vrserver.exe)...");
        // IL2CPP: Process.GetProcessesByName can return empty. Instead, retry
        // OpenVR.Init directly - it tells us whether the runtime is reachable.
        var err = EVRInitError.None;
        for (int i = 0;
 i < 60;
 i++)        {
            err = EVRInitError.None;
            OpenVR.Init(ref err, EVRApplicationType.VRApplication_Overlay);
            if (err == EVRInitError.None)            {
                steamVRRunning = true;
                Log($"OpenVR runtime connected (attempt {i + 1})");
                break;
            }
            yield return new UnityEngine.WaitForSeconds(1f);
        }
        if (!steamVRRunning)        {
            Log($"SteamVR NOT found after 60s (last init error: {err})");
            CurrentState = State.Error;
            Shutdown();
            yield break;
        }
        SetupAfterOpenVRInit();
    }
    // Runs once after a successful OpenVR.Init. Do NOT call OpenVR.Init again:
    // a second VR_Init in the same process without VR_Shutdown returns
    // Init_ClientVersionAlreadyProvided.
    void SetupAfterOpenVRInit()    {
        if (OpenVR.Compositor == null)            Log("VRCompositor() returned null!");
        else            Log("VRCompositor() OK");
        var manifestPath = Application.dataPath + "/../app.vrmanifest";
        var mErr = OpenVR.Applications.AddApplicationManifest(manifestPath, false);
        Log($"AddApplicationManifest: {mErr}");
        openVRReady = true;
        HideWindow();
        InitInputActions();
        if (!CreateOverlays())        {
            Log("Overlay creation failed, shutting down");
            Shutdown();
            return;
        }
        SetupRenderPipeline();
        UpdateInputComponent();
        StartCoroutine(CaptureDiagnosticOnce());
        SetPanelStatus("● 就绪");
        UpdateHint();
        Log("InitOpenVR complete");
    }
    void HideWindow()    {
        // Disabled: the Unity window stays visible on the desktop and shows the
        // panel via ScreenMirror (see SetupRenderPipeline). Moving it off-screen
        // left users staring at a black window.
    }
    IEnumerator CaptureDiagnosticOnce()    {
        if (uiCamera == null) yield break;
        uiCamera.enabled = true;
        yield return new WaitForEndOfFrame();
        CaptureRT();
        uiCamera.enabled = false;
    }
    private Texture2D captureTex2D;
    private int captureFrameCounter = 0;
    private bool textDiagLogged = false;
    byte[] CaptureRTToPngBytes()    {
        if (renderTexture == null || !renderTexture.IsCreated()) return null;
        var old = RenderTexture.active;
        RenderTexture.active = renderTexture;
        if (captureTex2D == null)            captureTex2D = new Texture2D(textureWidth, textureHeight, TextureFormat.RGBA32, false);
        captureTex2D.ReadPixels(new Rect(0, 0, textureWidth, textureHeight), 0, 0);
        captureTex2D.Apply();
        RenderTexture.active = old;
        return captureTex2D.EncodeToPNG();
    }
    void CaptureRT(string fileName = "rt_capture.png")    {
        var png = CaptureRTToPngBytes();
        if (png == null) return;
        var path = Application.dataPath + "/../" + fileName;
        try        {
            System.IO.File.WriteAllBytes(path, png);
        }
        catch (Exception e)        {
            Log($"RT capture write failed: {e.Message}");
        }
        var pixels = captureTex2D.GetPixels();
        int n = pixels.Length;
        // Subsample to keep the stats loop cheap (every 16th pixel).
        int step = 16;
        int count = 0;
        double r = 0, g = 0, b = 0, a = 0;
        int cyan = 0, yellow = 0, red = 0;
        for (int i = 0; i < n; i += step)        {
            var p = pixels[i];
            r += p.r;
            g += p.g;
            b += p.b;
            a += p.a;
            if (p.b > 0.5f && p.g > 0.4f && p.r < 0.4f) cyan++;
            if (p.r > 0.7f && p.g > 0.7f && p.b < 0.3f) yellow++;
            if (p.r > 0.8f && p.g < 0.4f && p.b < 0.4f) red++;
            count++;
        }
        var canvas = GameObject.Find("HandPanelCanvas")?.GetComponent<Canvas>();
        string canvasInfo = "canvas=null";
        if (canvas != null)        {
            int canvasLayer = canvas.gameObject.layer;
            bool canvasActive = canvas.isActiveAndEnabled;
            var wc = canvas.worldCamera;
            string wcInfo = wc != null ? wc.name + (wc.enabled ? "(enabled)" : "(disabled)") : "null";
            canvasInfo = $"canvas layer={canvasLayer} active={canvasActive} mode={canvas.renderMode} worldCamera={wcInfo} scaler={(canvas.GetComponent<CanvasScaler>() != null ? "yes" : "no")}";
        }
        Log($"RT: avg=({(r / count):F3},{(g / count):F3},{(b / count):F3},{(a / count):F3}) cyan={cyan} yellow={yellow} red={red} cam={uiCamera.name}:en={uiCamera.enabled}:mask={uiCamera.cullingMask} {canvasInfo}");
    }
    bool CreateOverlays()    {
        if (!openVRReady) return false;
        var uid = SystemInfo.deviceUniqueIdentifier;
        if (string.IsNullOrEmpty(uid)) uid = Guid.NewGuid().ToString("N");
        var uniqueKey = overlayKey + "_" + uid.Substring(0, 8);
        
// 1. Hand-attached world overlay
ulong existing = 0;
        var findErr = OpenVR.Overlay.FindOverlay(uniqueKey, ref existing);
        if (findErr == EVROverlayError.None && existing != 0)        {
            var dErr = OpenVR.Overlay.DestroyOverlay(existing);
            Log($"Destroyed stale hand overlay {existing}: {dErr}");
        }
        var err = OpenVR.Overlay.CreateOverlay(uniqueKey, overlayName, ref handOverlayHandle);
        Log($"CreateOverlay(hand, {uniqueKey}): {err}, handle={handOverlayHandle}");
        if (err != EVROverlayError.None)        {
            CurrentState = State.Error;
            SetPanelStatus("● 错误");
            Log($"[M] CreateOverlay failed: {err}");
            return false;
        }
        OpenVR.Overlay.SetOverlayFlag(handOverlayHandle, VROverlayFlags.NoDashboardTab, true);
        OpenVR.Overlay.SetOverlayWidthInMeters(handOverlayHandle, overlayWidthMeters);
        OpenVR.Overlay.SetOverlayInputMethod(handOverlayHandle, VROverlayInputMethod.Mouse);
        OpenVR.Overlay.SetOverlayFlag(handOverlayHandle, VROverlayFlags.MakeOverlaysInteractiveIfVisible, false);
        OpenVR.Overlay.SetOverlayFlag(handOverlayHandle, VROverlayFlags.SendVRSmoothScrollEvents, true);
        OpenVR.Overlay.SetOverlayAlpha(handOverlayHandle, overlayAlpha);
        var mouseScale = new HmdVector2_t {
 v0 = textureWidth, v1 = textureHeight }
;
        OpenVR.Overlay.SetOverlayMouseScale(handOverlayHandle, ref mouseScale);
        
// 2. Dashboard overlay
var dashKey = overlayKey + "_dashboard";
        ulong existingDash = 0;
        var findErrDash = OpenVR.Overlay.FindOverlay(dashKey, ref existingDash);
        if (findErrDash == EVROverlayError.None && existingDash != 0)        {
            var dErrDash = OpenVR.Overlay.DestroyOverlay(existingDash);
            Log($"Destroyed stale dashboard overlay {existingDash}: {dErrDash}");
        }
        var errDash = OpenVR.Overlay.CreateDashboardOverlay(dashKey, overlayName, ref dashboardOverlayHandle, ref dashboardThumbHandle);
        Log($"CreateDashboardOverlay(dashboard, {dashKey}): {errDash}, main={dashboardOverlayHandle}, thumb={dashboardThumbHandle}");
        if (errDash == EVROverlayError.None)        {
            OpenVR.Overlay.SetOverlayWidthInMeters(dashboardOverlayHandle, 1.5f);
            OpenVR.Overlay.SetOverlayInputMethod(dashboardOverlayHandle, VROverlayInputMethod.Mouse);
            OpenVR.Overlay.SetOverlayAlpha(dashboardOverlayHandle, overlayAlpha);
            var dashMouseScale = new HmdVector2_t {
 v0 = textureWidth, v1 = textureHeight }
;
            OpenVR.Overlay.SetOverlayMouseScale(dashboardOverlayHandle, ref dashMouseScale);
            SetupDashboardThumb();
        }
        UpdateHandAttachment();
        CreateLaserOverlay();
        Log("Overlays setup done");
        return true;
    }
    // Configure the dashboard tab thumbnail with a procedurally drawn icon. Without a
    // texture and size, SteamVR cannot read the thumb's metrics and logs
    // "Couldn't retrieve overlay metrics" every few minutes.
    void SetupDashboardThumb()    {
        if (dashboardThumbHandle == 0) return;
        OpenVR.Overlay.SetOverlayWidthInMeters(dashboardThumbHandle, 0.12f);
        var icon = CreateDashboardThumbIcon();
        if (icon == null) return;
        var pixels = icon.GetRawTextureData();
        var pHandle = System.Runtime.InteropServices.Marshal.AllocHGlobal(pixels.Length);
        try        {
            System.Runtime.InteropServices.Marshal.Copy(pixels, 0, pHandle, pixels.Length);
            var sErr = OpenVR.Overlay.SetOverlayRaw(dashboardThumbHandle, pHandle, (uint)icon.width, (uint)icon.height, 4u);
            Log($"SetOverlayRaw(thumb={dashboardThumbHandle}): {sErr}, {icon.width}x{icon.height}");
            if (sErr == EVROverlayError.None)        {
                var bounds = new VRTextureBounds_t        {
                    uMin = 0, vMin = 0, uMax = 1, vMax = 1            }
;
                OpenVR.Overlay.SetOverlayTextureBounds(dashboardThumbHandle, ref bounds);
            }
        }
        finally        {
            System.Runtime.InteropServices.Marshal.FreeHGlobal(pHandle);
        }
    }
    // Draw a simple Aoi-style icon: dark rounded circle with a white chat bubble.
    Texture2D CreateDashboardThumbIcon()    {
        const int size = 256;
        var tex = new Texture2D(size, size, TextureFormat.RGBA32, false);
        var colors = new Color32[size * size];
        var bg = new Color32(0x1F, 0x2A, 0x44, 0xFF);
        var fg = new Color32(0xFF, 0xFF, 0xFF, 0xFF);
        var glow = new Color32(0x2E, 0x8B, 0xE6, 0xFF);
        var cx = size / 2f;
        var cy = size / 2f;
        var radius = size * 0.42f;
        for (int y = 0; y < size; y++)        {
            for (int x = 0; x < size; x++)        {
                float dx = x + 0.5f - cx;
                float dy = y + 0.5f - cy;
                float dist = Mathf.Sqrt(dx * dx + dy * dy);
                var c = new Color32(0, 0, 0, 0);
                if (dist <= radius)        {
                    c = bg;
                    float edge = radius - dist;
                    if (edge < 4f) c = Color32.Lerp(new Color32(0, 0, 0, 0), bg, Mathf.Clamp01(edge / 4f));
                }
                // Chat bubble: rounded rectangle body
                float bx = Mathf.Abs(x - cx);
                float by = Mathf.Abs(y - (cy - 6));
                float bw = size * 0.24f;
                float bh = size * 0.20f;
                float r = size * 0.06f;
                float insideX = bx - (bw - r);
                float insideY = by - (bh - r);
                float m = Mathf.Min(Mathf.Max(insideX, insideY), 0) + Mathf.Sqrt(Mathf.Max(insideX, 0) * Mathf.Max(insideX, 0) + Mathf.Max(insideY, 0) * Mathf.Max(insideY, 0));
                if (m <= r)        {
                    float aa = Mathf.Clamp01(r - m);
                    if (aa > 0.5f) c = fg; else c = Color32.Lerp(bg, fg, aa * 2f);
                }
                // Bubble tail
                float tx = x - (cx - bw * 0.6f);
                float ty = y - (cy + 6 + bh * 0.7f);
                if (tx >= -size * 0.04f && tx <= size * 0.10f && ty >= -size * 0.02f && ty <= size * 0.08f && dist <= radius)        {
                    c = fg;
                }
                // Dots inside bubble
                float dotR = size * 0.016f;
                for (int i = -1; i <= 1; i++)        {
                    float ddx = x - (cx - i * size * 0.09f);
                    float ddy = y - (cy - 6);
                    if (ddx * ddx + ddy * ddy <= dotR * dotR && dist <= radius) c = glow;
                }
                colors[y * size + x] = c;
            }
        }
        tex.SetPixels32(colors);
        tex.Apply();
        return tex;
    }
    void SetupRenderPipeline()    {
        if (renderTexture == null)        {
            renderTexture = new RenderTexture(textureWidth, textureHeight, 0, RenderTextureFormat.ARGB32);
            renderTexture.Create();
            PanelTexWidth = textureWidth;
            PanelTexHeight = textureHeight;
        }
        if (uiCamera == null)        {
            var go = new GameObject("UICamera");
            go.transform.SetParent(transform);
            uiCamera = go.AddComponent<Camera>();
            uiCamera.clearFlags = CameraClearFlags.SolidColor;
            // Transparent clear: the panel's cut corners must stay alpha=0 in the
            // render texture so the overlay shows holes (see-through) at the corners.
            uiCamera.backgroundColor = new Color(0f, 0f, 0f, 0f);
            uiCamera.orthographic = true;
            uiCamera.orthographicSize = 0.5f;
            uiCamera.nearClipPlane = -1;
            uiCamera.farClipPlane = 1;
            // Render ONLY the hand panel (UI layer). Everything would render
            // scene geometry (walls, controllers, etc.) into the panel's render
            // texture and leak it into the VR overlay.
            uiCamera.cullingMask = 1 << LayerMask.NameToLayer("UI");
            uiCamera.targetTexture = renderTexture;
            uiCamera.depth = 100;
            var baseProj = uiCamera.projectionMatrix;
            uiCamera.projectionMatrix = Matrix4x4.Scale(new Vector3(1, -1, 1)) * baseProj;
            Log($"UICamera Y-flipped projection set (overlay display flip compensation)");
        }
        uiCamera.enabled = false;
        // Screen mirror: show the panel render texture in the Unity game view
        // (desktop window) so it is never black. The window stays visible now
        // (HideWindow disabled); this camera renders nothing itself and just
        // replaces the output with the panel RT via OnRenderImage.
        if (GameObject.Find("ScreenMirrorCamera") == null)
        {
            var mirrorGO = new GameObject("ScreenMirrorCamera");
            mirrorGO.transform.SetParent(transform);
            var mirrorCam = mirrorGO.AddComponent<Camera>();
            mirrorCam.clearFlags = CameraClearFlags.SolidColor;
            mirrorCam.backgroundColor = new Color(0.02f, 0.04f, 0.06f);
            mirrorCam.orthographic = true;
            mirrorCam.cullingMask = 0;
            mirrorCam.depth = -5;
            var mirror = mirrorGO.AddComponent<ScreenMirror>();
            mirror.source = renderTexture;
            WindowMirrorActive = true;
            Log("ScreenMirror camera created (panel shown in desktop window)");
        }
        var canvas = GameObject.Find("HandPanelCanvas")?.GetComponent<Canvas>();
        if (canvas != null)        {
            canvas.renderMode = RenderMode.WorldSpace;
            canvas.worldCamera = uiCamera;
            var crt = canvas.GetComponent<RectTransform>();
            // The UI is authored in absolute px on a 1024 board (matching the
            // mockup 1:1); map that space onto the 1x1 world unit.
            const float board = 684f;
            crt.sizeDelta = new Vector2(board, board);
            crt.localScale = new Vector3(1f / board, 1f / board, 1f);
            crt.position = uiCamera.transform.position + uiCamera.transform.forward * 0.5f;
            crt.rotation = uiCamera.transform.rotation;
            Log($"Canvas set to WorldSpace: board=({board}x{board}) scale={crt.localScale.x:F5} worldSize=1.0 rect={crt.rect}");
        }
        else        {
            Log("HandPanelCanvas not found!");
        }
    }
    void LogTextDiagnostics()    {
        try        {
            var settings = TMP_Settings.instance;
            Log($"TMP Settings: {(settings != null ? settings.name : "NULL")}");
            var canvas = GameObject.Find("HandPanelCanvas");
            if (canvas != null)            {
                var tmps = canvas.GetComponentsInChildren<TextMeshProUGUI>(true);
                foreach (var t in tmps)                {
                    try                    {
                        t.ForceMeshUpdate();
                        var mat = t.fontMaterial;
                        var tex = mat != null ? mat.mainTexture : null;
                        var rawText = t.text != null && t.text.Length > 12 ? t.text.Substring(0, 12) : t.text;
                        Log($"TMP UI '{t.gameObject.name}': font={(t.font != null ? t.font.name : "NULL")} chars={t.textInfo.characterCount} verts={t.textInfo.meshInfo[0].vertexCount} cull={t.canvasRenderer.cull} mat={(mat != null ? mat.name : "NULL")} tex={(tex != null ? tex.name + " " + tex.width + "x" + tex.height : "NULL")} active={t.isActiveAndEnabled} canvasGroup={(t.canvas != null ? t.canvas.name : "NULL")} text=\"{rawText}\"");
                    }
                    catch (Exception ex)                    {
                        Log($"TMP UI '{t.gameObject.name}' diag failed: {ex.Message}");
                    }
                }
            }
            var t3d = GetComponentInChildren<TextMeshPro>();
            if (t3d != null)            {
                var mat = t3d.fontMaterial;
                var tex = mat != null ? mat.mainTexture : null;
                var mr = t3d.GetComponent<MeshRenderer>();
                var mf = t3d.GetComponent<MeshFilter>();
                Log($"TMP 3D: font={(t3d.font != null ? t3d.font.name : "NULL")} chars={t3d.textInfo.characterCount} verts={t3d.textInfo.meshInfo[0].vertexCount} renderer={mr != null && mr.enabled} meshFilter={(mf != null && mf.mesh != null ? mf.mesh.vertexCount.ToString() : "NULL")} shader={(mat != null ? mat.shader.name : "NULL")} mat={(mat != null ? mat.name : "NULL")} tex={(tex != null ? tex.name + " " + tex.width + "x" + tex.height : "NULL")}");
            }
        }
        catch (Exception e)        {
            Log($"Text diagnostics failed: {e.Message}");
        }
    }
    void UpdateInputComponent()    {
        inputComponent = FindFirstObjectByType<SteamVROverlayInput>();
        if (inputComponent != null)        {
            inputComponent.textureWidth = textureWidth;
            inputComponent.textureHeight = textureHeight;
        }
    }
    private float lastVRInteractionTime = 0f;
    private float lastChatUpdateTime = 0f;
    private float lastGripReleaseTime = -100f;
    private float gripPressTime = -100f;
    private float lastHoldReleaseTime = -100f;
    private float lastTransformErrLog = -100f;
    private bool gripHeldRecording = false;
    private bool inputActionsReady = false;
    private ulong gripActionSetHandle = 0;
    private ulong gripActionHandle = 0;
    private ulong gripLeftHandle = 0;
    private ulong gripRightHandle = 0;
    private int actionDiagCounter = 0;
    // Dual-grip gesture (both hands held = screenshot + record; right release stops)
    private bool dualShotRecording = false;
    private float bothHoldStart = -1f;
    private const float DualHoldSeconds = 0.4f;
    // Background feature states (independent of mic state, drive status-bar chips)
    private bool interpActive = false;
    private bool envActive = false;
    void Update()    {
        // Demo recording: capture frames from the app's OWN render output
        // (never the desktop) at ~10 fps, then quit after 30s.
        if (DemoRecordActive)        {
            if (demoRecDir == null)            {
                demoRecDir = Application.dataPath + "/../demo_rec";
                if (!System.IO.Directory.Exists(demoRecDir))
                    System.IO.Directory.CreateDirectory(demoRecDir);
                demoRecStart = Time.unscaledTime;
                Log("[Demo] recording -> " + demoRecDir);
            }
            if (Time.unscaledTime - demoRecStart > 30f)            {
                Log($"[Demo] recording done ({demoRecFrames} frames)");
                Application.Quit();
                return;
            }
            demoRecTimer -= Time.unscaledTime - demoRecLast;
            demoRecLast = Time.unscaledTime;
            if (demoRecTimer <= 0f)            {
                demoRecTimer = 0.1f; // ~10 fps
                demoRecFrames++;
                ScreenCapture.CaptureScreenshot(demoRecDir + "/frame_" + demoRecFrames.ToString("0000") + ".png");
            }
        }
        // Drive the processing pipeline fade-out.
        var procUI = GetPanelUI();
        if (procUI != null) procUI.UpdateProcessing();
        if (DesktopMode)        {
            while (mainThreadActions.TryDequeue(out var dAction))                dAction();
            return;
        }
        if (!openVRReady) return;
        ProcessOverlayEvents();
        ProcessSystemButtonEvents();
        CheckPendingScreenshot();
        RefreshTrackedPoses();
        UpdateHandAttachment();
        UpdateGripInput();
        UpdateLaser();
        ProcessLaserClick();
        if (calling && panelAutoHideSeconds > 0 && Time.unscaledTime - lastVRInteractionTime > panelAutoHideSeconds)        {
            HidePanel();
            Log($"Panel auto-hidden after {panelAutoHideSeconds}s idle");
        }
        bool dashVisible = dashboardOverlayHandle != 0 && OpenVR.Overlay.IsOverlayVisible(dashboardOverlayHandle);
        if (dashVisible != dashWasVisible)            Log($"Dashboard overlay visible: {dashVisible}");
        dashWasVisible = dashVisible;
        bool needRender = overlayVisible || dashVisible;
        // With the desktop window mirror active the panel must render
        // continuously (not only while the VR overlay is visible), otherwise
        // the window shows a stale/black frame until the user looks at the
        // panel in VR first.
        uiCamera.enabled = needRender || WindowMirrorActive;
        Application.targetFrameRate = (needRender || WindowMirrorActive) ? 30 : 10;
        if (needRender)        {
            if (Time.frameCount % 3 == 0)            {
                if (overlayVisible) SubmitTexture(handOverlayHandle);
                if (dashVisible)
                {
                    SubmitTexture(dashboardOverlayHandle);
                }
            }
            if (++captureFrameCounter % 3600 == 0)            {
                CaptureRT();
                if (!textDiagLogged) {
 textDiagLogged = true;
 LogTextDiagnostics();
 }
            }
        }
        while (mainThreadActions.TryDequeue(out var action))            action();
    }
    void ProcessOverlayEvents()    {
        var ev = new VREvent_t();
        uint size = (uint)Marshal.SizeOf(typeof(VREvent_t));
        while (handOverlayHandle != 0 && OpenVR.Overlay.PollNextOverlayEvent(handOverlayHandle, ref ev, size))            HandleOverlayEvent(ev);
        while (dashboardOverlayHandle != 0 && OpenVR.Overlay.PollNextOverlayEvent(dashboardOverlayHandle, ref ev, size))            HandleOverlayEvent(ev);
    }
    void HandleOverlayEvent(VREvent_t ev)    {
        var evType = (EVREventType)ev.eventType;
        if (evType == EVREventType.VREvent_MouseButtonDown || evType == EVREventType.VREvent_MouseButtonUp)            lastVRInteractionTime = Time.unscaledTime;
        switch (evType)        {
            case EVREventType.VREvent_Quit:                Log("SteamVR quit requested");
                Shutdown();
                Application.Quit();
                break;
            default:                inputComponent?.HandleVREvent(ev);
                break;
        }
    }
    void RefreshTrackedPoses()    {
        if (laserPoseBuffer == null)            laserPoseBuffer = new TrackedDevicePose_t[OpenVR.k_unMaxTrackedDeviceCount];
        OpenVR.System.GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin.TrackingUniverseStanding, 0, laserPoseBuffer);
    }
    void UpdateHandAttachment()    {
        if (handOverlayHandle == 0) return;
        var rightIndex = OpenVR.System.GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole.RightHand);
        if (rightIndex == OpenVR.k_unTrackedDeviceIndexInvalid) return;
        var offset = new HmdMatrix34_t        {
            m0 = 1, m1 = 0, m2 = 0, m3 = handOffsetRight,            m4 = 0, m5 = 0, m6 = 1, m7 = handOffsetUp,            m8 = 0, m9 = -1, m10 = 0, m11 = handOffsetForward        }
;
        // Use an ABSOLUTE transform so ComputeOverlayIntersection (used by the
        // custom laser) can see the panel. Compose the right controller's pose
        // with the tracked-device-relative offset matrix every frame.
        var rp = laserPoseBuffer != null ? laserPoseBuffer[rightIndex] : default;
        if (!rp.bPoseIsValid) return;
        var abs = Mul(rp.mDeviceToAbsoluteTracking, offset);
        var tErr = OpenVR.Overlay.SetOverlayTransformAbsolute(handOverlayHandle, ETrackingUniverseOrigin.TrackingUniverseStanding, ref abs);
        if (tErr != EVROverlayError.None && tErr != EVROverlayError.InvalidParameter)        {
            float nowT = Time.unscaledTime;
            if (nowT - lastTransformErrLog > 5f)            {
                lastTransformErrLog = nowT;
                Log($"SetOverlayTransformAbsolute: {tErr}");
            }
        }
    }

    // 3x4 row-major matrix multiply: a * b (treat 3x4 as 4x4 with bottom row [0,0,0,1])
    static HmdMatrix34_t Mul(HmdMatrix34_t a, HmdMatrix34_t b)    {
        var r = new HmdMatrix34_t();
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 4; j++)
            {
                float s = 0;
                for (int k = 0; k < 3; k++)
                    s += Get(a, i, k) * Get(b, k, j);
                if (j == 3) s += Get(a, i, 3); // a[i][3] * b[3][3] where b[3][3]=1
                Set(ref r, i, j, s);
            }
        }
        return r;
    }
    static float Get(HmdMatrix34_t m, int row, int col)
    {
        switch (row * 4 + col)
        {
            case 0: return m.m0; case 1: return m.m1; case 2: return m.m2; case 3: return m.m3;
            case 4: return m.m4; case 5: return m.m5; case 6: return m.m6; case 7: return m.m7;
            case 8: return m.m8; case 9: return m.m9; case 10: return m.m10; case 11: return m.m11;
        }
        return 0;
    }
    static void Set(ref HmdMatrix34_t m, int row, int col, float v)
    {
        switch (row * 4 + col)
        {
            case 0: m.m0 = v; break; case 1: m.m1 = v; break; case 2: m.m2 = v; break; case 3: m.m3 = v; break;
            case 4: m.m4 = v; break; case 5: m.m5 = v; break; case 6: m.m6 = v; break; case 7: m.m7 = v; break;
            case 8: m.m8 = v; break; case 9: m.m9 = v; break; case 10: m.m10 = v; break; case 11: m.m11 = v; break;
        }
    }

    // Custom laser pointer. We draw our own thin overlay beam instead of using
    // SteamVR's laser mouse mode (MakeOverlaysInteractiveIfVisible), which would
    // always show a laser from the right controller since the panel is attached
    // to it. The beam originates from the LEFT controller and is only visible
    // while its ray actually hits the panel.
    void CreateLaserOverlay()    {
        if (laserOverlayHandle != 0) return;
        var key = overlayKey + "_laser";
        ulong existing = 0;
        var fErr = OpenVR.Overlay.FindOverlay(key, ref existing);
        if (fErr == EVROverlayError.None && existing != 0)            OpenVR.Overlay.DestroyOverlay(existing);
        var err = OpenVR.Overlay.CreateOverlay(key, "Aoi Laser", ref laserOverlayHandle);
        if (err != EVROverlayError.None)        {
            Log($"CreateOverlay(laser): {err}");
            return;
        }
        OpenVR.Overlay.SetOverlayWidthInMeters(laserOverlayHandle, laserThicknessMeters);
        OpenVR.Overlay.SetOverlayInputMethod(laserOverlayHandle, VROverlayInputMethod.None);
        OpenVR.Overlay.SetOverlayFlag(laserOverlayHandle, VROverlayFlags.NoDashboardTab, true);
        // Draw the laser above other overlays so it's always visible.
        OpenVR.Overlay.SetOverlaySortOrder(laserOverlayHandle, 2);

        // Use SetOverlayRaw with a tiny solid-color texture. The 2x2 white square is
        // verified to render (the debug big square used it). SteamVR loads this
        // asynchronously and supports only small raw buffers.
        var pixels = new byte[2 * 2 * 4];
        for (int i = 0; i < 2 * 2; i++)        {
            pixels[i * 4 + 0] = 0xFF; // R
            pixels[i * 4 + 1] = 0xFF; // G
            pixels[i * 4 + 2] = 0xFF; // B
            pixels[i * 4 + 3] = 0xFF; // A
        }
        var pHandle = System.Runtime.InteropServices.Marshal.AllocHGlobal(pixels.Length);
        try        {
            System.Runtime.InteropServices.Marshal.Copy(pixels, 0, pHandle, pixels.Length);
            var sErr = OpenVR.Overlay.SetOverlayRaw(laserOverlayHandle, pHandle, 2, 2, 4);
            if (sErr != EVROverlayError.None)            Log($"SetOverlayRaw(laser): {sErr}");
        }
        finally        {
            System.Runtime.InteropServices.Marshal.FreeHGlobal(pHandle);
        }
        OpenVR.Overlay.HideOverlay(laserOverlayHandle);
        Log("Laser overlay created");
    }

    void UpdateLaser()    {
        if (handOverlayHandle == 0 || laserOverlayHandle == 0) return;
        var leftIndex = OpenVR.System.GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole.LeftHand);
        if (leftIndex == OpenVR.k_unTrackedDeviceIndexInvalid)        {
            SetLaserBeamVisible(false);
            return;
        }
        var pose = laserPoseBuffer != null ? laserPoseBuffer[leftIndex] : default;
        if (!pose.bPoseIsValid || !pose.bDeviceIsConnected)        {
            SetLaserBeamVisible(false);
            return;
        }
        // All math below uses OpenVR native coordinates (no z-flip).
        var mat = pose.mDeviceToAbsoluteTracking;
        var source = new HmdVector3_t        {
            v0 = mat.m3, v1 = mat.m7, v2 = mat.m11 }
;
        // Controller local basis in world (row-major columns).
        var xAxis = new HmdVector3_t        {
            v0 = mat.m0, v1 = mat.m4, v2 = mat.m8 }
;
        var yAxis = new HmdVector3_t        {
            v0 = mat.m1, v1 = mat.m5, v2 = mat.m9 }
;
        var zAxis = new HmdVector3_t        {
            v0 = mat.m2, v1 = mat.m6, v2 = mat.m10 }
;
        // OpenVR forward is the controller's local -Z (row-major matrix row 3).
        // DesktopPlus uses exactly this: forward = -(m2,m6,m10) and vSource = pose
        // translation, with ComputeOverlayIntersection for hit testing.
        var dir = new HmdVector3_t        {
            v0 = -zAxis.v0, v1 = -zAxis.v1, v2 = -zAxis.v2 }
;

        // Compute intersection from the left controller toward the panel. The panel
        // uses an ABSOLUTE transform now, so ComputeOverlayIntersection works.
        laserHitThisFrame = false;
        float dist = 0f;
        var hitUv = new HmdVector2_t();
        try        {
            var p = new VROverlayIntersectionParams_t        {
                vSource = source,                vDirection = dir,                eOrigin = ETrackingUniverseOrigin.TrackingUniverseStanding        }
;
            var res = new VROverlayIntersectionResults_t();
            if (OpenVR.Overlay.ComputeOverlayIntersection(handOverlayHandle, ref p, ref res))
            {
                laserHitThisFrame = true;
                dist = res.fDistance;
                hitUv = res.vUVs;
            }
        }
        catch (Exception e)        {
            Log("UpdateLaser error: " + e.Message);
        }
        lastLaserHit.vUVs = hitUv;
        lastLaserHit.fDistance = dist;
        if (!laserHitThisFrame)        {
            SetLaserBeamVisible(false);
            return;
        }
        SetLaserBeamVisible(true, dist, source, dir);
    }

    static float Dot(HmdVector3_t a, HmdVector3_t b)    {
        return a.v0 * b.v0 + a.v1 * b.v1 + a.v2 * b.v2;
    }

    void SetLaserBeamVisible(bool on, float distance = 1f, HmdVector3_t source = default, HmdVector3_t dir = default)    {
        if (on)        {
            if (!laserVisible)            {
                var showErr = OpenVR.Overlay.ShowOverlay(laserOverlayHandle);
                if (showErr != EVROverlayError.None)            {
                    Log($"ShowOverlay(laser): {showErr}");
                    return;
                }
                laserVisible = true;
            }
            OpenVR.Overlay.SetOverlayAlpha(laserOverlayHandle, 1.0f);
            // SteamVR-aligned laser thickness (2mm); the old hardcoded 0.01m
            // (1cm) beam was visibly thicker than SteamVR's own laser.
            float beamWidth = laserThicknessMeters;
            OpenVR.Overlay.SetOverlayWidthInMeters(laserOverlayHandle, beamWidth);
            var bounds = new VRTextureBounds_t        {
                uMin = 0f,                vMin = 0f,                uMax = 1f,                vMax = Mathf.Max(1f, distance / beamWidth)        }
;
            OpenVR.Overlay.SetOverlayTextureBounds(laserOverlayHandle, ref bounds);

            // Position along the LEFT controller's forward using an ABSOLUTE transform
            // (the same approach that rendered the beam in front of the HMD). Matrix is
            // rotation+translation only; overlay Y = controller forward.
            var leftIndex = OpenVR.System.GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole.LeftHand);
            if (leftIndex == OpenVR.k_unTrackedDeviceIndexInvalid)            {
                OpenVR.Overlay.HideOverlay(laserOverlayHandle);
                laserVisible = false;
                return;
            }
            var leftPose = laserPoseBuffer != null && leftIndex < laserPoseBuffer.Length ? laserPoseBuffer[leftIndex] : default;
            if (!leftPose.bPoseIsValid)            {
                OpenVR.Overlay.HideOverlay(laserOverlayHandle);
                laserVisible = false;
                return;
            }
            var lm = leftPose.mDeviceToAbsoluteTracking;
            var src = new HmdVector3_t        {
                v0 = lm.m3, v1 = lm.m7, v2 = lm.m11 }
;
            var fwd = new HmdVector3_t        {
                v0 = -lm.m2, v1 = -lm.m6, v2 = -lm.m10 }
;
            var half = new HmdVector3_t        {
                v0 = src.v0 + fwd.v0 * distance * 0.5f,                v1 = src.v1 + fwd.v1 * distance * 0.5f,                v2 = src.v2 + fwd.v2 * distance * 0.5f        }
;
            // Build an orthonormal basis: Y = forward (beam length axis), Z = face
            // normal pointing at the HMD, X = Y x Z.
            var hmdMat = laserPoseBuffer != null && laserPoseBuffer.Length > 0 ? laserPoseBuffer[0].mDeviceToAbsoluteTracking : default;
            var toHmd = new HmdVector3_t        {
                v0 = hmdMat.m3 - half.v0, v1 = hmdMat.m7 - half.v1, v2 = hmdMat.m11 - half.v2        }
;
            var z = new HmdVector3_t { v0 = toHmd.v0, v1 = toHmd.v1, v2 = toHmd.v2 };
            // Remove component parallel to fwd so z is the face normal in the beam's cross-section.
            var along = Dot(z, fwd);
            z = new HmdVector3_t        {
                v0 = z.v0 - fwd.v0 * along, v1 = z.v1 - fwd.v1 * along, v2 = z.v2 - fwd.v2 * along        }
;
            if (Len(z) < 1e-4f) z = new HmdVector3_t { v0 = 0, v1 = 1, v2 = 0 };
            z = Normalize(z);
            var x = Cross(fwd, z);
            x = Normalize(x);
            var beam = new HmdMatrix34_t        {
                m0 = x.v0, m1 = fwd.v0, m2 = z.v0, m3 = half.v0,                m4 = x.v1, m5 = fwd.v1, m6 = z.v1, m7 = half.v1,                m8 = x.v2, m9 = fwd.v2, m10 = z.v2, m11 = half.v2        }
;
            OpenVR.Overlay.SetOverlayTransformAbsolute(laserOverlayHandle, ETrackingUniverseOrigin.TrackingUniverseStanding, ref beam);
        }
        else if (laserVisible)        {
            OpenVR.Overlay.HideOverlay(laserOverlayHandle);
            laserVisible = false;
        }
    }

    static HmdVector3_t Cross(HmdVector3_t a, HmdVector3_t b)    {
        return new HmdVector3_t        {
            v0 = a.v1 * b.v2 - a.v2 * b.v1,            v1 = a.v2 * b.v0 - a.v0 * b.v2,            v2 = a.v0 * b.v1 - a.v1 * b.v0        }
;
    }

    static float Len(HmdVector3_t a)    {
        return Mathf.Sqrt(a.v0 * a.v0 + a.v1 * a.v1 + a.v2 * a.v2);
    }

    static HmdVector3_t Normalize(HmdVector3_t a)    {
        var l = Len(a);
        if (l < 1e-6f) return new HmdVector3_t();
        return new HmdVector3_t        {
            v0 = a.v0 / l, v1 = a.v1 / l, v2 = a.v2 / l        }
;
    }

    // Returns true if the left controller trigger is currently pressed.
    bool IsLeftTriggerPressed()    {
        if (!inputActionsReady || triggerActionHandle == 0) return false;
        gripActionSets[0].ulActionSet = gripActionSetHandle;
        gripActionSets[0].ulRestrictedToDevice = 0;
        if (OpenVR.Input.UpdateActionState(gripActionSets, (uint)Marshal.SizeOf(typeof(VRActiveActionSet_t))) != EVRInputError.None) return false;
        var data = new InputDigitalActionData_t();
        var err = OpenVR.Input.GetDigitalActionData(triggerActionHandle, ref data, (uint)Marshal.SizeOf(typeof(InputDigitalActionData_t)), 0);
        return err == EVRInputError.None && data.bState;
    }

    void ProcessLaserClick()    {
        if (!laserHitThisFrame)        {
            if (triggerWasPressed)        {
                if (inputComponent != null) inputComponent.EndDrag();
            }
            triggerWasPressed = false;
            return;
        }
        bool pressed = IsLeftTriggerPressed();
        var u = lastLaserHit.vUVs.v0;
        var v = lastLaserHit.vUVs.v1;
        if (++actionDiagCounter % 30 == 0)        {
            Log($"[Click] hit={laserHitThisFrame} pressed={pressed} triggerWas={triggerWasPressed} uv=({u:F2},{v:F2})");
        }
        var input = inputComponent;
        if (pressed && !triggerWasPressed)        {
            Log($"Laser click at uv=({u:F3},{v:F3})");
            if (input != null)            {
                input.SimulateClick(u, v);
            }
        }
        else if (pressed && triggerWasPressed)        {
            // Held down: keep updating the pointer position so a drag on the
            // ScrollRect follows the laser movement.
            if (input != null) input.UpdatePointer(u, v);
        }
        else if (!pressed && triggerWasPressed)        {
            if (input != null) input.EndDrag();
        }
        triggerWasPressed = pressed;
    }

    void DestroyLaserOverlay()    {
        if (laserOverlayHandle != 0)        {
            OpenVR.Overlay.DestroyOverlay(laserOverlayHandle);
            laserOverlayHandle = 0;
        }
        laserVisible = false;
    }
    void ProcessSystemButtonEvents()    {
        var ev = new VREvent_t();
        uint size = (uint)Marshal.SizeOf(typeof(VREvent_t));
        while (OpenVR.System.PollNextEvent(ref ev, size))        {
            try            {
                var evType = (EVREventType)ev.eventType;
                if (evType == EVREventType.VREvent_ScreenshotTaken)                {
                    Log($"OpenVR screenshot taken (handle={ev.data.screenshot.handle})");
                    if (pendingScreenshotId != null && ev.data.screenshot.handle == pendingScreenshotHandle)                        TrySendPendingScreenshot();
                    continue;
                }
                if (evType == EVREventType.VREvent_ScreenshotFailed)                {
                    Log($"OpenVR screenshot FAILED (handle={ev.data.screenshot.handle})");
                    if (pendingScreenshotId != null && ev.data.screenshot.handle == pendingScreenshotHandle)                        SendScreenshotError(pendingScreenshotId);
                    pendingScreenshotId = null;
                    pendingScreenshotHandle = 0;
                    continue;
                }
                if (evType == EVREventType.VREvent_ButtonPress)                {
                    var btn = ev.data.controller.button;
                    if (btn == (uint)EVRButtonId.k_EButton_Grip || btn == (uint)EVRButtonId.k_EButton_ApplicationMenu)                    {
                        if (inputActionsReady) continue; // action path (UpdateGripInput) handles it; avoid double-fire
                        lastVRInteractionTime = Time.unscaledTime;
                        float now = Time.unscaledTime;
                        if (now - lastGripReleaseTime < 0.3f)                        {
                            lastGripReleaseTime = -100f;
                            TogglePanelNoRecord();
                        }
                    }
                    continue;
                }
                if (evType == EVREventType.VREvent_ButtonUnpress)                {
                    var btn = ev.data.controller.button;
                    if (btn == (uint)EVRButtonId.k_EButton_Grip || btn == (uint)EVRButtonId.k_EButton_ApplicationMenu)                    {
                        if (inputActionsReady) continue; // action path handles it
                        lastVRInteractionTime = Time.unscaledTime;
                        lastGripReleaseTime = Time.unscaledTime;
                    }
                    continue;
                }
            }
            catch (Exception e)            {
                Log("ProcessSystemButtonEvents error: " + e.Message + "\n" + e.StackTrace);
            }
        }
    }
    void InitInputActions()    {
        if (OpenVR.Input == null) {
 Log("OpenVR.Input unavailable");
 return;
 }
        var manifest = System.IO.Path.Combine(Application.streamingAssetsPath, "action_manifest.json");
        if (!System.IO.File.Exists(manifest)) {
 Log("Action manifest not found: " + manifest);
 return;
 }
        var err = OpenVR.Input.SetActionManifestPath(manifest);
        Log($"SetActionManifestPath: {err}");
        if (err != EVRInputError.None) return;
        err = OpenVR.Input.GetActionSetHandle("/actions/aoi", ref gripActionSetHandle);
        Log($"GetActionSetHandle(/actions/aoi): {err} handle={gripActionSetHandle}");
        err = OpenVR.Input.GetActionHandle("/actions/aoi/in/Grip", ref gripActionHandle);
        Log($"GetActionHandle(Grip): {err} handle={gripActionHandle}");
        err = OpenVR.Input.GetActionHandle("/actions/aoi/in/GripLeft", ref gripLeftHandle);
        Log($"GetActionHandle(GripLeft): {err} handle={gripLeftHandle}");
        err = OpenVR.Input.GetActionHandle("/actions/aoi/in/GripRight", ref gripRightHandle);
        Log($"GetActionHandle(GripRight): {err} handle={gripRightHandle}");
        err = OpenVR.Input.GetActionHandle("/actions/aoi/in/TriggerClick", ref triggerActionHandle);
        Log($"GetActionHandle(TriggerClick): {err} handle={triggerActionHandle}");
        inputActionsReady = gripActionSetHandle != 0 && gripLeftHandle != 0 && gripRightHandle != 0;
    }
    void UpdateGripInput()    {
        if (!inputActionsReady) return;
        gripActionSets[0].ulActionSet = gripActionSetHandle;
        // 0 = k_ulInvalidInputValueHandle = activate the set for ALL devices.
        // (This must be the invalid handle, NOT a tracked-device index like 0xFFFFFFFF.)
        gripActionSets[0].ulRestrictedToDevice = 0;
        var updErr = OpenVR.Input.UpdateActionState(gripActionSets, (uint)Marshal.SizeOf(typeof(VRActiveActionSet_t)));
        if (updErr != EVRInputError.None) return;

        // Per-hand actions bound explicitly to /user/hand/left|right in the SteamVR
        // bindings; reading with ulRestrictToDevice=0 (all devices) gives each hand's
        // state directly — no device-index/role lookups needed.
        bool left = ReadActionPressed(gripLeftHandle);
        bool right = ReadActionPressed(gripRightHandle);
        bool pressed = left || right;
        bool both = left && right;
        float now = Time.unscaledTime;

        if (pressed && !gripWasPressed) Log($"[Grip] press (left={(left ? 1 : 0)} right={(right ? 1 : 0)})");
        if (!pressed && gripWasPressed) Log("[Grip] release");
        if (++actionDiagCounter % 120 == 0)
            Log($"[Grip] probe left={(left ? 1 : 0)} right={(right ? 1 : 0)}");

        // ---- Dual-grip mode: recording; right-hand release stops it. ----
        if (dualShotRecording)        {
            if (!right)            {
                dualShotRecording = false;
                lastHoldReleaseTime = now;
                StopDualRecording();
            }
            gripWasPressed = pressed;
            return;
        }

        // ---- Dual-grip start: both grips held for DualHoldSeconds, ONLY while the
        // panel is open. When the panel is hidden, a long dual hold must do nothing. ----
        if (both && overlayVisible)        {
            if (bothHoldStart < 0f) bothHoldStart = now;
            if (now - bothHoldStart >= DualHoldSeconds)            {
                bothHoldStart = -1f;
                StartDualRecording();
                gripWasPressed = pressed;
                return;
            }
        }
        else            bothHoldStart = -1f;

        // ---- Single-hand logic: double-tap toggle for either hand; long-press
        // hold-to-record ONLY for the right hand (left-hand long-press does
        // nothing). Screenshot+recording still requires BOTH hands. ----
        if (!both)        {
            if (pressed && !gripWasPressed)        {
                bool afterRecordingRelease = now - lastHoldReleaseTime < 0.6f;
                bool isDoubleTap = !afterRecordingRelease && (now - lastGripReleaseTime < 0.3f);
                if (isDoubleTap)        {
                    lastGripReleaseTime = -100f;
                    gripHeldRecording = false;
                    gripPressTime = float.MinValue;
                    TogglePanelNoRecord();
                }
                else        {
                    gripPressTime = now;
                    gripHeldRecording = false;
                }
            }
            else if (pressed && gripWasPressed)        {
                if (right && !left && !gripHeldRecording && gripPressTime > 0 && Time.unscaledTime - gripPressTime > 0.4f && overlayVisible)        {
                    gripHeldRecording = true;
                    StartHoldRecording();
                }
            }
            else if (!pressed && gripWasPressed)        {
                lastGripReleaseTime = Time.unscaledTime;
                if (gripHeldRecording)        {
                    gripHeldRecording = false;
                    lastHoldReleaseTime = Time.unscaledTime;
                    StopHoldRecording();
                }
            }
        }
        gripWasPressed = pressed;
    }
    bool ReadActionPressed(ulong actionHandle)    {
        if (actionHandle == 0) return false;
        var d = new InputDigitalActionData_t();
        // ulRestrictToDevice = 0 (k_ulInvalidInputValueHandle) = query all devices.
        var e = OpenVR.Input.GetDigitalActionData(actionHandle, ref d, (uint)Marshal.SizeOf(typeof(InputDigitalActionData_t)), 0);
        return e == EVRInputError.None && d.bState;
    }
    void StartDualRecording()    {
        // Screenshot at press moment (mirror of what the user is looking at).
        string shotPath = null;
        try        {
            var png = CaptureMirrorToPng();
            if (png != null)            {
                shotPath = Application.dataPath + "/../shot_dual_" + System.DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".png";
                System.IO.File.WriteAllBytes(shotPath, png);
            }
        }
        catch (System.Exception e) { Log("Dual screenshot failed: " + e.Message); }

        dualShotRecording = true;
        gripHeldRecording = false;
        calling = true;
        CurrentState = State.Active;
        ShowOverlay();
        AgentSendTtsStop();
        if (shotPath != null)
            AgentSendStateChange("active", "shot", shotPath.Replace("\\", "\\\\"));
        else
            AgentSendStateChange("active", "shot", null);
        SetPanelStatus("● 截图+录音...", 1);
        var panel = GetPanelUI();
        if (panel != null) panel.ShowShotToast("已截图", "将随语音发送给 Aoi");
        UpdateHint();
        Log("Dual-grip: screenshot + recording started, shot=" + (shotPath ?? "FAILED"));
    }
    void StopDualRecording()    {
        calling = false;
        CurrentState = State.Standby;
        AgentSendStateChange("standby", "shot", null);
        SetPanelStatus("正在发送...", 1);
        UpdateHint();
        Log("Dual-grip release (right hand): audio+shot sent");
    }
    void StartHoldRecording()    {
        calling = true;
        CurrentState = State.Active;
        ShowOverlay();
        // Long-press grip hands the conversation floor back to the user: stop any
        // current + queued TTS playback on the agent before we start recording.
        AgentSendTtsStop();
        AgentSendStateChange("active", null, null);
        SetPanelStatus("● 正在录音...", 1);
        UpdateHint();
        Log("Hold: recording started");
    }
    void StopHoldRecording()    {
        calling = false;
        CurrentState = State.Standby;
        AgentSendStateChange("standby", null, null);
        SetPanelStatus("正在发送...", 1);
        UpdateHint();
        Log("Hold release: audio sent, panel stays open");
    }
    void ShowOverlay()    {
        if (handOverlayHandle != 0)        {
            if (uiCamera != null) uiCamera.enabled = true;
            var e = OpenVR.Overlay.ShowOverlay(handOverlayHandle);
            Log($"ShowOverlay: {e}");
            if (e == EVROverlayError.None)
            {
                overlayVisible = true;
                // We draw our own laser beam (UpdateLaser); system laser mouse mode stays off.
                OpenVR.Overlay.SetOverlayFlag(handOverlayHandle, VROverlayFlags.MakeOverlaysInteractiveIfVisible, false);
            }
        }
    }
    void HideOverlay()    {
        if (handOverlayHandle != 0)        {
            var e = OpenVR.Overlay.HideOverlay(handOverlayHandle);
            if (e == EVROverlayError.None)
            {
                overlayVisible = false;
                OpenVR.Overlay.SetOverlayFlag(handOverlayHandle, VROverlayFlags.MakeOverlaysInteractiveIfVisible, false);
            }
        }
    }
    public void HidePanel()    {
        if (!overlayVisible && !calling) return;
        var wasCalling = calling;
        calling = false;
        CurrentState = State.Standby;
        HideOverlay();
        // If the panel hides while recording, tell the agent to stop & process
        // (otherwise it waits forever for a standby that never comes).
        if (wasCalling)
            AgentSendStateChange("standby", null, null);
        SetPanelStatus("● 就绪");
        UpdateHint();
        Log("Panel hidden");
    }
    public void ShowPanel()    {
        if (overlayVisible) return;
        calling = false; // panel shown, NOT recording (state_change active = recording)
        CurrentState = State.Active;
        ShowOverlay();
        SetPanelStatus("● 就绪");
        UpdateHint();
        Log("Panel shown");
    }
    HandPanelUI GetPanelUI()
    {
        if (cachedPanelUI == null)
            cachedPanelUI = GameObject.Find("HandPanelCanvas")?.GetComponent<HandPanelUI>();
        return cachedPanelUI;
    }

    void SetPanelStatus(string text, int colorState = 0)    {
        var panelUI = GetPanelUI();
        if (panelUI != null) panelUI.SetStatus(text, colorState);
    }

    // Contextual hint bar: teaches the gestures available in the current state.
    void UpdateHint()    {
        var panel = GetPanelUI();
        if (panel == null) return;
        const string K = "<mark=#02D7F226> ";
        const string E = " </mark>";
        if (!overlayVisible && !calling && !dualShotRecording && !DesktopMode)        {
            panel.SetHint($"{K}双击 Grip{E} 呼出面板");
            return;
        }
        if (dualShotRecording)        {
            panel.SetHint($"{K}松开右手 Grip{E} 发送截图+语音");
            return;
        }
        if (gripHeldRecording)        {
            panel.SetHint($"{K}松开 Grip{E} 发送");
            return;
        }
        var sb = new System.Text.StringBuilder();
        sb.Append($"{K}双击 Grip{E} 隐藏面板   {K}按住 Grip{E} 说话   {K}双手按住 Grip{E} 截图+说话");
        if (interpActive)
            sb.Append($"\n{K}说「停止翻译」{E} 关闭字幕 · 按住 Grip 仍可直接提问");
        else if (envActive)
            sb.Append($"\n环境上下文中：持续截图 + 收听扬声器作为上下文 · {K}说「关闭环境上下文」{E} 停止");
        else
            sb.Append($"\n{K}说「开启环境上下文」{E} 持续感知画面与声音 · {K}说「开启实时字幕」{E} 翻译扬声器声音");
        panel.SetHint(sb.ToString());
    }
    void TogglePanelNoRecord()    {
        if (overlayVisible)        {
            // Same as HidePanel: if we were recording (holding Grip), tell the
            // agent to stop & process — otherwise it keeps its mic open forever.
            var wasCalling = calling;
            calling = false;
            CurrentState = State.Standby;
            HideOverlay();
            // Double-tap closes the panel: stop any current + queued TTS
            // playback (same as long-press recording hands the floor back
            // to the user).
            AgentSendTtsStop();
            if (wasCalling)
                AgentSendStateChange("standby", null, null);
            SetPanelStatus("● 就绪");
            UpdateHint();
            Log("Double-tap: panel hidden");
        }
        else        {
            calling = false;
            CurrentState = State.Active;
            ShowOverlay();
            SetPanelStatus("● 就绪");
            UpdateHint();
            Log("Double-tap: panel shown");
        }
    }
    public void OpenBindingSettings()    {
        try        {
            // The dashboard that follows will steal the pointer before SteamVR
            // delivers MouseButtonUp, leaving mousePressed stuck true. Clear it.
            inputComponent?.CancelPress();
            // Official IVRInput::OpenBindingUI - opens the SteamVR binding editor
            // for THIS application (by app_key), same as OVRAS does.
            if (OpenVR.Input != null && inputActionsReady)
            {
                var err = OpenVR.Input.OpenBindingUI(overlayKey, gripActionSetHandle, 0, false);
                Log($"OpenBindingUI({overlayKey}): {err}");
                if (err == EVRInputError.None) return;
                Log("OpenBindingUI failed, falling back to SteamVR settings: " + err);
            }
            System.Diagnostics.Process.Start("steamvr://open/settings");
            Log("Opened SteamVR settings (fallback)");
        }
        catch (Exception e)        {
            Log("Open binding settings failed: " + e.Message);
        }
    }
    void SubmitTexture(ulong handle)    {
        if (handle == 0) return;
        if (renderTexture == null || !renderTexture.IsCreated()) return;
        var nativePtr = renderTexture.GetNativeTexturePtr();
        if (nativePtr == IntPtr.Zero) return;
        var texture = new Texture_t        {
            handle = nativePtr,            eType = ETextureType.DirectX,            eColorSpace = EColorSpace.Auto        }
;
        var sErr = OpenVR.Overlay.SetOverlayTexture(handle, ref texture);
        if (sErr != EVROverlayError.None)            Log($"SetOverlayTexture({handle}): {sErr}");
        else if (!textureSubmitted)        {
            textureSubmitted = true;
            Log($"SetOverlayTexture({handle}): OK, ptr={nativePtr}");
        }
    }
    public void TakeScreenshot()    {
        try        {
            if (DesktopMode)            {
                var fileName = "screenshot_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".png";
                ScreenCapture.CaptureScreenshot(Application.dataPath + "/../" + fileName);
                ShowScreenshotToast();
                Log("Screenshot saved (desktop): " + fileName);
            }
            else if (openVRReady)            {
                var mirrorPng = CaptureMirrorToPng();
                if (mirrorPng != null)
                {
                    var dst = Application.dataPath + "/../screenshot_vr_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".png";
                    System.IO.File.WriteAllBytes(dst, mirrorPng);
                    Log("VR screenshot saved: " + dst);
                    return;
                }
                Log("Screenshot failed: mirror capture returned null");
            }
            else            {
                Log("Screenshot failed: OpenVR not ready");
            }
        }
        catch (Exception e)        {
            Log("Screenshot failed: " + e.Message);
        }
    }
    private string pendingScreenshotId = null;
    private uint pendingScreenshotHandle = 0;
    private string pendingScreenshotPath = null;
    private float pendingShotStartTime = -1f;
    private float lastScreenshotScanTime = -1f;
    private System.DateTime pendingShotWallTime = System.DateTime.MinValue;
    private int pendingShotRetries = 0;
    private bool screenshotNameLogged = false;
    string NextScreenshotPath()    {
        return Application.dataPath + "/../user_view.png";
    }
    public void TestScreenshotOnly()    {
        var png = CaptureMirrorToPng();
        if (png != null)
        {
            var dst = Application.dataPath + "/../test_shot.png";
            System.IO.File.WriteAllBytes(dst, png);
            Log("[T] mirror screenshot saved: " + dst);
        }
        else Log("[T] mirror capture failed");
    }
    [DllImport("d3d11.dll")]    private static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software, uint flags, int[] featureLevels, uint numLevels, uint sdkVersion, out IntPtr device, out IntPtr featureLevel, out IntPtr context);
        [DllImport("MirrorCapture.dll", CharSet = CharSet.Unicode)]
    private static extern int MirrorShot(IntPtr device, IntPtr ctx, IntPtr srv, string outBmpPath);

        void EnsureClickAudio()
    {
        if (clickAudio != null) return;
        clickAudio = gameObject.AddComponent<AudioSource>();
        clickAudio.playOnAwake = false;
        int sampleRate = 44100;
        float duration = 0.04f;
        int samples = (int)(sampleRate * duration);
        var data = new float[samples];
        for (int i = 0; i < samples; i++)
        {
            float t = (float)i / samples;
            float env = Mathf.Exp(-t * 50f);
            data[i] = (Mathf.Sin(2 * Mathf.PI * 1500f * t) * 0.6f + Mathf.Sin(2 * Mathf.PI * 3200f * t) * 0.4f) * env;
        }
        clickClip = AudioClip.Create("screenshot_click", samples, 1, sampleRate, false);
        clickClip.SetData(data, 0);
    }

    void PlayClickSound()
    {
        try
        {
            EnsureClickAudio();
            clickAudio.PlayOneShot(clickClip);
        }
        catch (Exception e)
        {
            Log("[M] click sound failed: " + e.Message);
        }
    }

    void ShowScreenshotToast()
    {
        try
        {
            var panel = GetPanelUI();
            if (panel != null) panel.ShowShotToast("已截图");
        }
        catch (Exception e)
        {
            Log("[M] toast failed: " + e.Message);
        }
    }

    private static IntPtr cachedDevice = IntPtr.Zero;
    private static IntPtr cachedCtx = IntPtr.Zero;
    private static IntPtr cachedMirrorSrv = IntPtr.Zero;

    // Lightweight mirror grab (main thread only): raw BGRA pixels, no encoding,
    // no sound/toast. Heavy encode/scale/write work is done on worker threads.
    public byte[] CaptureMirrorRaw(out int width, out int height)
    {
        width = 0; height = 0;
        var result = CaptureMirrorRawOnce(out width, out height);
        if (result != null) return result;
        // SteamVR may have crashed and restarted, invalidating the cached D3D
        // device + mirror SRV. Rebuild the caches once and retry before giving up.
        // Debounce: only rebuild if we haven't just rebuilt in the last 5s, so a
        // persistent failure (e.g. no HMD) doesn't churn device re-creation.
        if (Time.unscaledTime - lastMirrorCacheReset > 5f)
        {
            Log("[M] mirror capture failed, rebuilding cached device/SRV and retrying");
            lastMirrorCacheReset = Time.unscaledTime;
            ResetMirrorCache();
            width = 0; height = 0;
            return CaptureMirrorRawOnce(out width, out height);
        }
        return null;
    }

    private static float lastMirrorCacheReset = -999f;

    private static void ResetMirrorCache()
    {
        if (cachedMirrorSrv != IntPtr.Zero && OpenVR.Compositor != null)
        {
            try { OpenVR.Compositor.ReleaseMirrorTextureD3D11(cachedMirrorSrv); } catch { }
        }
        cachedMirrorSrv = IntPtr.Zero;
        if (cachedDevice != IntPtr.Zero)
        {
            try { Marshal.Release(cachedDevice); } catch { }
        }
        cachedDevice = IntPtr.Zero;
        cachedCtx = IntPtr.Zero;
    }

    private byte[] CaptureMirrorRawOnce(out int width, out int height)
    {
        width = 0; height = 0;
        try
        {
            IntPtr device, featureLevel, ctx;
            if (cachedDevice != IntPtr.Zero)
            {
                device = cachedDevice;
                ctx = cachedCtx;
            }
            else
            {
                int hr = D3D11CreateDevice(IntPtr.Zero, 1, IntPtr.Zero, 0, null, 0, 7, out device, out featureLevel, out ctx);
                if (hr != 0)
                {
                    Log("[M] D3D11CreateDevice failed: " + hr);
                    return null;
                }
                cachedDevice = device;
                cachedCtx = ctx;
            }

            IntPtr srv = cachedMirrorSrv;
            if (srv == IntPtr.Zero)            {
                var err0 = OpenVR.Compositor.GetMirrorTextureD3D11(EVREye.Eye_Left, device, ref srv);
                if (err0 != EVRCompositorError.None || srv == IntPtr.Zero)            {
                    Log("[M] GetMirrorTextureD3D11 failed: " + err0);
                    return null;
                }
                cachedMirrorSrv = srv;
                // GetMirrorTextureD3D11 should be called ONCE per eye and the SRV
                // reused. Releasing/re-acquiring each frame makes SteamVR serve
                // stale textures for a few seconds (openvr issue #1888).
            }

            var bmpPath = Application.dataPath + "/../mirror_shot.bmp";
            int rc = MirrorShot(device, ctx, srv, bmpPath);
            if (rc != 0)            {
                Log("[M] MirrorShot failed rc=" + rc);
                return null;
            }
            if (!System.IO.File.Exists(bmpPath))
            {
                Log("[M] bmp not written");
                return null;
            }

            var data = System.IO.File.ReadAllBytes(bmpPath);
            int w = BitConverter.ToInt32(data, 18);
            int h = BitConverter.ToInt32(data, 22);
            if (w <= 0 || h <= 0)
            {
                Log("[M] bmp invalid size");
                return null;
            }

            var pixels = new byte[w * h * 4];
            for (int y = 0; y < h; y++)
            {
                int srcRow = 54 + y * w * 4;
                Buffer.BlockCopy(data, srcRow, pixels, y * w * 4, w * 4);
            }
            System.IO.File.Delete(bmpPath);
            width = w; height = h;
            return pixels;
        }
        catch (Exception e)
        {
            Log("[M] mirror raw error: " + e.Message);
            return null;
        }
    }

    public byte[] CaptureMirrorToPng()
    {
        var bgra = CaptureMirrorRaw(out int w, out int h);
        if (bgra == null) return null;
        try
        {
            var tex = new Texture2D(w, h, TextureFormat.BGRA32, false);
            tex.LoadRawTextureData(bgra);
            tex.Apply();
            var png = tex.EncodeToPNG();
            Destroy(tex);
            Log($"[M] mirror capture ok: {w}x{h} ({png.Length} bytes)");
            PlayClickSound();
            ShowScreenshotToast();
            return png;
        }
        catch (Exception e)
        {
            Log("[M] mirror encode error: " + e.Message);
            return null;
        }
    }

    // ---- Environment context capture (awareness) ----
    private Coroutine captureCoroutine = null;
    private byte[] lastFrameJpg = null;
    private string contextFramesDir = null;
    private readonly object frameLock = new object();
    private int lastAwarenessDiagTick = 0;
    private const int FrameMaxWidth = 384;

    void StartContinuousCapture()
    {
        if (captureCoroutine != null) return;
        if (contextFramesDir == null)
        {
            contextFramesDir = Application.dataPath + "/../context/frames";
            System.IO.Directory.CreateDirectory(contextFramesDir);
        }
        captureCoroutine = StartCoroutine(ContinuousCaptureLoop());
        Log("[AWARENESS] continuous capture started");
    }

    void StopContinuousCapture()
    {
        if (captureCoroutine != null)
        {
            StopCoroutine(captureCoroutine);
            captureCoroutine = null;
        }
        lock (frameLock) lastFrameJpg = null;
        Log("[AWARENESS] continuous capture stopped");
    }

    System.Collections.IEnumerator ContinuousCaptureLoop()
    {
        while (true)
        {
            if (DesktopMode)
                yield return CaptureFrameDesktopOnce();
            else
                CaptureFrameOnce();
            yield return new WaitForSeconds(1f); // 1 fps
        }
    }

    // Desktop-mode frame capture: no OpenVR mirror here, grab the game view instead.
    System.Collections.IEnumerator CaptureFrameDesktopOnce()
    {
        var tmp = System.IO.Path.Combine(contextFramesDir, "_tmp_shot.png");
        ScreenCapture.CaptureScreenshot(tmp);
        yield return null;
        yield return new WaitForEndOfFrame();
        try
        {
            if (!System.IO.File.Exists(tmp)) yield break;
            var png = System.IO.File.ReadAllBytes(tmp);
            System.IO.File.Delete(tmp);
            var tex = new Texture2D(2, 2, TextureFormat.RGBA32, false);
            if (!tex.LoadImage(png)) { Destroy(tex); yield break; }
            int w = tex.width, h = tex.height;
            var src = tex.GetRawTextureData();
            Destroy(tex);
            var enc = ScaleEncodePng(src, w, h, FrameMaxWidth, false);
            if (enc == null) yield break;
            if (lastFrameJpg != null && FrameSimilar(enc, lastFrameJpg)) yield break;
            lastFrameJpg = enc;
            var path = System.IO.Path.Combine(contextFramesDir, "frame_" + DateTime.Now.ToString("yyyyMMdd_HHmmss_fff") + ".png");
            System.IO.File.WriteAllBytes(path, enc);
            Log("[AWARENESS] frame saved (desktop): " + path + " (" + enc.Length + "B)");
        }
        catch (Exception e)
        {
            Log("[AWARENESS] desktop capture error: " + e.Message);
        }
    }

    void CaptureFrameOnce()
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var bgra = CaptureMirrorRaw(out int w, out int h);
        if (bgra == null)
        {
            Log("[AWARENESS] mirror grab failed");
            return;
        }
        var png = ScaleEncodePng(bgra, w, h, FrameMaxWidth, true);
        sw.Stop();
        if (png == null || png.Length == 0) return;
        lock (frameLock)
        {
            if (lastFrameJpg != null && FrameSimilar(png, lastFrameJpg))
            {
                if (AwarenessDiagDue()) Log("[AWARENESS] frame unchanged, skipped");
                return;
            }
            lastFrameJpg = png;
        }
        var path = System.IO.Path.Combine(contextFramesDir, "frame_" + DateTime.Now.ToString("yyyyMMdd_HHmmss_fff") + ".png");
        System.IO.File.WriteAllBytes(path, png);
        if (AwarenessDiagDue()) Log("[AWARENESS] frame saved: " + path + " (" + png.Length + "B) " + sw.ElapsedMilliseconds + "ms");
    }

    // Diag logging gate shared by main + worker threads (thread-safe via tick).
    bool AwarenessDiagDue()
    {
        int now = Environment.TickCount;
        if (now - lastAwarenessDiagTick >= 30000)
        {
            lastAwarenessDiagTick = now;
            return true;
        }
        return false;
    }

    // CPU bilinear downscale + PNG encode. Must run on the main thread: the only
    // reliable encoder in this Unity build is Texture2D + Apply + EncodeToPNG
    // (GPU-backed); ImageConversion.EncodeArray* and LoadRawTextureData-without-
    // Apply both return corrupt/empty output. 384-wide PNG encode is cheap
    // (~10-20ms), so the main-thread cost per second is acceptable. Output is
    // RGBA (4 bytes/px). swapBR=true for BGRA mirror input (R/B swap fix);
    // false for RGBA input (desktop LoadImage path).
    byte[] ScaleEncodePng(byte[] src, int srcW, int srcH, int maxWidth, bool swapBR)
    {
        float scale = Mathf.Min(1f, (float)maxWidth / srcW);
        int nw = Mathf.Max(1, Mathf.RoundToInt(srcW * scale));
        int nh = Mathf.Max(1, Mathf.RoundToInt(srcH * scale));
        var rgba = new byte[nw * nh * 4];
        for (int y = 0; y < nh; y++)
        {
            float sy = (y + 0.5f) * srcH / nh;
            int sy0 = Mathf.Clamp((int)sy, 0, srcH - 1);
            int sy1 = Mathf.Clamp(sy0 + 1, 0, srcH - 1);
            float fy = sy - sy0;
            for (int x = 0; x < nw; x++)
            {
                float sx = (x + 0.5f) * srcW / nw;
                int sx0 = Mathf.Clamp((int)sx, 0, srcW - 1);
                int sx1 = Mathf.Clamp(sx0 + 1, 0, srcW - 1);
                float fx = sx - sx0;
                int i00 = (sy0 * srcW + sx0) * 4;
                int i01 = (sy0 * srcW + sx1) * 4;
                int i10 = (sy1 * srcW + sx0) * 4;
                int i11 = (sy1 * srcW + sx1) * 4;
                int o = (y * nw + x) * 4;
                for (int c = 0; c < 4; c++)
                {
                    float v = (src[i00 + c] * (1f - fx) + src[i01 + c] * fx) * (1f - fy)
                            + (src[i10 + c] * (1f - fx) + src[i11 + c] * fx) * fy;
                    rgba[o + c] = (byte)(v + 0.5f);
                }
                if (swapBR)
                {
                    byte t = rgba[o];
                    rgba[o] = rgba[o + 2];
                    rgba[o + 2] = t;
                }
            }
        }
        var tex = new Texture2D(nw, nh, TextureFormat.RGBA32, false);
        tex.LoadRawTextureData(rgba);
        tex.Apply();
        var png = tex.EncodeToPNG();
        Destroy(tex);
        return png;
    }

    // Cheap perceptual diff on downsampled pixels: sample a grid and compare RGB.
    bool FrameSimilar(byte[] a, byte[] b)
    {
        if (a == null || b == null || a.Length != b.Length) return false;
        // Compare a coarse grid of bytes (step ~1/64 of data) to bound cost.
        int step = Mathf.Max(1, a.Length / 4096);
        int diff = 0, n = 0;
        for (int i = 0; i + 2 < a.Length; i += step)
        {
            int dr = Mathf.Abs(a[i] - b[i]);
            int dg = Mathf.Abs(a[i + 1] - b[i + 1]);
            int db = Mathf.Abs(a[i + 2] - b[i + 2]);
            if (dr + dg + db > 30) diff++;
            n++;
        }
        // Unchanged if fewer than 1.5% of sampled bytes differ.
        return (float)diff / Mathf.Max(1, n) < 0.015f;
    }

    public void TestMirrorScreenshot()
    {
        var png = CaptureMirrorToPng();
        if (png != null)
        {
            var path = Application.dataPath + "/../mirror_shot.png";
            System.IO.File.WriteAllBytes(path, png);
            Log("[M] mirror shot saved: " + path);
        }
    }
void TakeScreenshotForAgent(string requestId)    {
        try        {
            Log($"Screenshot for agent: id={requestId}");
            if (DesktopMode)            {
                StartCoroutine(DesktopScreenshotForAgent(requestId));
                return;
            }
            if (openVRReady)            {
                var mirrorPng = CaptureMirrorToPng();
                if (mirrorPng != null)
                {
                    var dst = Application.dataPath + "/../screenshot_vr_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".png";
                    System.IO.File.WriteAllBytes(dst, mirrorPng);
                    SendScreenshotPathResponse(requestId, dst);
                    return;
                }
                if (pendingScreenshotId != null)                {
                    Log("Screenshot already in progress, replying error to avoid agent hang");
                    SendScreenshotError(requestId);
                    return;
                }
                pendingScreenshotId = requestId;
                pendingScreenshotPath = null;
                pendingShotStartTime = Time.unscaledTime;
                pendingShotWallTime = System.DateTime.UtcNow;
                screenshotNameLogged = false;
                uint handle = 0;
                var err = OpenVR.Screenshots.RequestScreenshot(ref handle, EVRScreenshotType.Stereo, "", "");
                Log($"OpenVR RequestScreenshot: {err} handle={handle}");
                if (err == EVRScreenshotError.None)                {
                    pendingScreenshotHandle = handle;
                    return;
                }
                pendingScreenshotId = null;
            }
            var png = CaptureRTToPngBytes();
            if (png != null)                SendScreenshotResponse(requestId, png);
            else
            {
                Log("Screenshot for agent failed: no image");
                SendScreenshotError(requestId);  // don't leave the agent hanging 30s
            }
        }
        catch (Exception e)        {
            Log("Screenshot for agent failed: " + e.Message);
            SendScreenshotError(requestId);
        }
    }
    IEnumerator DesktopScreenshotForAgent(string requestId)    {
        var path = Application.dataPath + "/../agent_shot.png";
        ScreenCapture.CaptureScreenshot(path);
        yield return null;
        yield return new WaitForEndOfFrame();
        try        {
            if (System.IO.File.Exists(path))            {
                var bytes = System.IO.File.ReadAllBytes(path);
                SendScreenshotResponse(requestId, bytes);
                ShowScreenshotToast();
            }
            else            {
                SendScreenshotError(requestId);
            }
        }
        catch (Exception e)        {
            Log("Desktop screenshot for agent failed: " + e.Message);
            SendScreenshotError(requestId);
        }
    }
    bool TryReadScreenshotFile(uint handle, out byte[] bytes, out string path)    {
        bytes = null;
        path = null;
        try        {
            var sb = new System.Text.StringBuilder(1024);
            var err = EVRScreenshotError.None;
            uint len = OpenVR.Screenshots.GetScreenshotPropertyFilename(handle, EVRScreenshotPropertyFilenames.Preview, sb, 1024, ref err);
            if (screenshotNameLogged == false)            {
                Log($"GetScreenshotPropertyFilename(Preview): len={len} err={err} path='{sb}'");
                screenshotNameLogged = true;
            }
            if (len == 0)            {
                sb.Length = 0;
                len = OpenVR.Screenshots.GetScreenshotPropertyFilename(handle, EVRScreenshotPropertyFilenames.VR, sb, 1024, ref err);
                if (screenshotNameLogged == false)                {
                    Log($"GetScreenshotPropertyFilename(VR): len={len} err={err} path='{sb}'");
                    screenshotNameLogged = true;
                }
            }
            if (len == 0 || sb.Length == 0) return false;
            var p = sb.ToString();
            if (!System.IO.File.Exists(p)) return false;
            path = p;
            bytes = System.IO.File.ReadAllBytes(p);
            Log($"Read SteamVR screenshot: {p} ({bytes.Length} bytes)");
            return true;
        }
        catch (Exception e)        {
            Log("Read screenshot failed: " + e.Message);
            return false;
        }
    }
    void TrySendPendingScreenshot()    {
        if (pendingScreenshotId == null) return;
        try        {
            byte[] bytes = null;
            string srcPath = null;
            if (pendingScreenshotPath != null && System.IO.File.Exists(pendingScreenshotPath))            {
                srcPath = pendingScreenshotPath;
                bytes = System.IO.File.ReadAllBytes(srcPath);
                Log($"Read screenshot from custom path: {srcPath} ({bytes.Length} bytes)");
            }
            else if (TryReadScreenshotFile(pendingScreenshotHandle, out bytes, out srcPath))            {
            }
            else if (TryFindSteamVRShotInTemp(out bytes, out srcPath))            {
            }
            else            {
                return;
            }
            var dst = Application.dataPath + "/../screenshot_vr_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".png";
            try            {
                System.IO.File.WriteAllBytes(dst, bytes);
                Log("VR screenshot saved: " + dst);
            }
            catch (Exception e)            {
                Log("Copy screenshot failed: " + e.Message);
            }
            if (pendingScreenshotId.StartsWith("manual_"))            {
                Log("Manual screenshot saved: " + dst);
            }
            else            {
                SendScreenshotPathResponse(pendingScreenshotId, dst);
            }
            pendingScreenshotId = null;
            pendingScreenshotHandle = 0;
            pendingScreenshotPath = null;
        }
        catch (Exception e)        {
            Log("TrySendPendingScreenshot error: " + e.Message + "\n" + e.StackTrace);
        }
    }
    bool TryFindSteamVRShotInTemp(out byte[] bytes, out string path)    {
        bytes = null;
        path = null;
        try        {
            var tempDir = System.IO.Path.GetTempPath();
            var newest = GetNewestScreenshotInDir(tempDir, pendingShotStartTime);
            if (newest == null)            {
                var docs = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "SteamVR", "Screenshots");
                newest = GetNewestScreenshotInDir(docs, pendingShotStartTime);
            }
            if (newest == null) return false;
            path = newest;
            var b1 = System.IO.File.ReadAllBytes(path);
            System.Threading.Thread.Sleep(50);
            if (!System.IO.File.Exists(path)) return false;
            var b2 = System.IO.File.ReadAllBytes(path);
            if (b1.Length != b2.Length) return false;
            bytes = b2;
            Log($"Read SteamVR screenshot from scan: {path} ({bytes.Length} bytes)");
            return true;
        }
        catch (Exception e)        {
            Log("Scan temp screenshot failed: " + e.Message);
            return false;
        }
    }
    string GetNewestScreenshotInDir(string dir, float sinceTime)    {
        if (!System.IO.Directory.Exists(dir)) return null;
        string newest = null;
        System.DateTime newestTime = System.DateTime.MinValue;
        foreach (var f in System.IO.Directory.GetFiles(dir, "Screenshot_*.png"))        {
            var fi = new System.IO.FileInfo(f);
            if (fi.LastWriteTimeUtc > pendingShotWallTime && fi.LastWriteTime > newestTime)            {
                newestTime = fi.LastWriteTime;
                newest = f;
            }
        }
        return newest;
    }
    void CheckPendingScreenshot()    {
        if (pendingScreenshotId == null) return;
        // Throttle the %TEMP% directory scan — doing it every frame (60fps)
        // with Directory.GetFiles + FileInfo churns allocations and drops frames
        // for up to 20s while waiting for SteamVR to produce the file.
        if (Time.unscaledTime - lastScreenshotScanTime < 0.3f) return;
        lastScreenshotScanTime = Time.unscaledTime;
        if (Time.unscaledTime - pendingShotStartTime > 0.5f)        {
            TrySendPendingScreenshot();
        }
        if (pendingScreenshotId != null && Time.unscaledTime - pendingShotStartTime > 20f)        {
            if (pendingShotRetries < 2)            {
                pendingShotRetries++;
                Log($"Screenshot timeout, retrying request ({pendingShotRetries}/2)");
                pendingShotStartTime = Time.unscaledTime;
                pendingShotWallTime = System.DateTime.UtcNow;
                uint handle = 0;
                var err = OpenVR.Screenshots.RequestScreenshot(ref handle, EVRScreenshotType.Stereo, "", "");
                Log($"OpenVR RequestScreenshot (retry {pendingShotRetries}): {err} handle={handle}");
                if (err == EVRScreenshotError.None)                    pendingScreenshotHandle = handle;
                else                {
                    SendScreenshotError(pendingScreenshotId);
                    pendingScreenshotId = null;
                    pendingScreenshotHandle = 0;
                    pendingScreenshotPath = null;
                    pendingShotRetries = 0;
                }
                return;
            }
            Log("OpenVR screenshot timeout after retries, sending error");
            try            {
                var cancelErr = OpenVR.Screenshots.SubmitScreenshot(pendingScreenshotHandle, EVRScreenshotType.Stereo, "", "");
                Log($"SubmitScreenshot(cancel limbo): {cancelErr}");
            }
            catch (Exception e)            {
                Log("Cancel limbo failed: " + e.Message);
            }
            if (pendingScreenshotId.StartsWith("manual_"))                Log("VR screenshot failed: no file produced");
            else                SendScreenshotError(pendingScreenshotId);
            pendingScreenshotId = null;
            pendingScreenshotHandle = 0;
            pendingScreenshotPath = null;
            pendingShotRetries = 0;
        }
    }
    void SendScreenshotError(string requestId)    {
        AgentSendScreenshotError(requestId, "screenshot_failed");
        Log($"Screenshot error sent to agent (id={requestId})");
    }
    void SendScreenshotPathResponse(string requestId, string path)    {
        AgentSendScreenshotPath(requestId, path);
        Log($"Screenshot path sent to agent (id={requestId}, {path})");
    }
    void SendScreenshotResponse(string requestId, byte[] png)    {
        var jpg = CompressScreenshot(png, 1024, 70);
        var b64 = Convert.ToBase64String(jpg);
        AgentSendScreenshotImage(requestId, b64);
        Log($"Screenshot response sent to agent (id={requestId}, {jpg.Length} bytes)");
    }
    byte[] CompressScreenshot(byte[] png, int maxSize, int quality)    {
        Texture2D tex = null;
        try        {
            tex = new Texture2D(2, 2, TextureFormat.RGB24, false);
            if (!tex.LoadImage(png)) return png;
            int w = tex.width, h = tex.height;
            float scale = Mathf.Min(1f, (float)maxSize / Mathf.Max(w, h));
            int nw = Mathf.Max(1, Mathf.RoundToInt(w * scale));
            int nh = Mathf.Max(1, Mathf.RoundToInt(h * scale));
            if (nw != w || nh != h)            {
                var rt = RenderTexture.GetTemporary(nw, nh);
                try                {
                    Graphics.Blit(tex, rt);
                    var dst = new Texture2D(nw, nh, TextureFormat.RGB24, false);
                    RenderTexture.active = rt;
                    try                {
                        dst.ReadPixels(new Rect(0, 0, nw, nh), 0, 0);
                        dst.Apply();
                    }
                    finally                {
                        RenderTexture.active = null;
                    }
                    RenderTexture.ReleaseTemporary(rt);
                    Destroy(tex);
                    tex = dst;
                }
                catch                {
                    RenderTexture.ReleaseTemporary(rt);
                    throw;
                }
            }
            var jpg = tex.EncodeToJPG(quality);
            return jpg;
        }
        catch (Exception e)        {
            Log("CompressScreenshot failed: " + e.Message);
            return png;
        }
        finally        {
            if (tex != null)            {
                if (Application.isPlaying) Destroy(tex);
                else UnityEngine.Object.DestroyImmediate(tex);
            }
        }
    }
    // ---- AgentSend* helpers: thin semantic wrappers over the DLL bridge.
    // The message schema (type/payload/timestamp/id JSON) lives entirely in the
    // C++ DLL; C# only passes raw values. No JSON is built here. ----
    void AgentSendTtsStop()    {
        var b = FindFirstObjectByType<AoiNativeAgent>();
        if (b != null) b.SendTtsStop();
    }
    void AgentSendStateChange(string state, string mode, string shotPath)    {
        var b = FindFirstObjectByType<AoiNativeAgent>();
        if (b != null) b.SendStateChange(state, mode, shotPath);
    }
    void AgentSendScreenshotPath(string requestId, string path)    {
        var b = FindFirstObjectByType<AoiNativeAgent>();
        if (b != null) b.SendScreenshotPath(requestId, path);
    }
    void AgentSendScreenshotImage(string requestId, string b64)    {
        var b = FindFirstObjectByType<AoiNativeAgent>();
        if (b != null) b.SendScreenshotImage(requestId, b64);
    }
    void AgentSendScreenshotError(string requestId, string error)    {
        var b = FindFirstObjectByType<AoiNativeAgent>();
        if (b != null) b.SendScreenshotError(requestId, error);
    }
    void AgentSendDisplayResult(bool success)    {
        var b = FindFirstObjectByType<AoiNativeAgent>();
        if (b != null) b.SendDisplayResult(success);
    }
    // Inbound messages from the embedded C++ agent (called on the Unity main
    // thread by AoiNativeAgent).
    public void HandleAgentMessage(string json)    {
        EnqueueMainThreadAction(() =>        {
            try            {
                var rawType = ExtractJsonField(json, "type");
                Log($"[AGENT->UI] received type={rawType} len={json.Length}");
                var type = ExtractJsonField(json, "type");
                var payload = ExtractJsonField(json, "payload");
                switch (type)                {
                    case "processing_stage":
                        var stage = ExtractJsonField(payload, "stage");
                        var thought = ExtractJsonField(payload, "thought");
                        var procUI = GetPanelUI();
                        if (procUI != null) procUI.SetProcessingStage(stage, thought);
                        break;
                    case "assistant_response":                        var text = ExtractJsonField(payload, "text");
                        var partial = ExtractJsonField(payload, "partial") == "true";
                        Log($"[Aoi] {text}");
                        if (!string.IsNullOrEmpty(text))                        {
                            var panelUI = GetPanelUI();
                            bool isError = IsErrorText(text);
                            if (panelUI != null && (!partial || Time.unscaledTime - lastChatUpdateTime > 0.2f))                            {
                                lastChatUpdateTime = Time.unscaledTime;
                                if (isError && !partial) panelUI.AppendErrorText(text);
                                else panelUI.AppendChatText(text, !partial);
                            }
                        }
                        if (!partial)                        {
                            // Reply complete (normal or error): mic back to ready,
                            // processing bar reset immediately (covers lost "done"
                            // on error paths).
                            SetPanelStatus("● 就绪");
                            UpdateHint();
                            var puiDone = GetPanelUI();
                            if (puiDone != null) puiDone.ResetProcessing();
                        }
                        break;
                    case "translation":                        var ttext = ExtractJsonField(payload, "text");
                        Log($"[TRANSLATION] {ttext}");
                        if (!string.IsNullOrEmpty(ttext))                        {
                            var panelUI = GetPanelUI();
                            if (panelUI != null) panelUI.AddSubtitle(ttext);
                        }
                        break;
                    case "interpretation_state":                        var active = ExtractJsonField(payload, "active") == "true";
                        var targetLang = ExtractJsonField(payload, "target_lang");
                        Log($"[INTERP] active={active} target={targetLang}");
                        interpActive = active;
                        var pui = GetPanelUI();
                        if (pui != null)
                        {
                            pui.SetSubtitleActive(active);
                            // Chip carries the state; main status text stays mic-state only.
                            pui.SetInterpActive(active, targetLang);
                        }
                        UpdateHint();
                        break;
                    case "stream_text":                        var stext = ExtractJsonField(payload, "text");
                        var seq = ExtractJsonField(payload, "seq");
                        if (!string.IsNullOrEmpty(stext))                        {
                            var panelUI2 = GetPanelUI();
                            if (panelUI2 != null) panelUI2.SetChatText(stext);
                            Log($"[Aoi stream {seq}] {stext}");
                        }
                        break;
                    case "display":                        var clear = ExtractJsonField(payload, "clear") == "true";
                        var content = ExtractJsonField(payload, "content");
                        var panelUI3 = GetPanelUI();
                        if (panelUI3 != null)                        {
                            if (clear) panelUI3.SetChatText("");
                            else if (!string.IsNullOrEmpty(content)) panelUI3.SetChatText(content);
                        }
                        AgentSendDisplayResult(true);
                        Log($"[Aoi display] {(clear ? "(clear)" : content)}");
                        break;
                    case "test_mirror":                        Log("[TEST] test_mirror received");
                        TestMirrorScreenshot();
                        break;
                    case "test_screenshot":                        Log("[TEST] test_screenshot received");
                        TestScreenshotOnly();
                        break;
                    case "screenshot_request":                    case "screenshot":                        Log("[Aoi] Screenshot requested");
                        var reqId = ExtractJsonField(json, "id");
                        TakeScreenshotForAgent(string.IsNullOrEmpty(reqId) ? Guid.NewGuid().ToString("N") : reqId);
                        break;
                    case "vr_skill_request":                    case "vr_skill":                        {
                            var skill = ExtractJsonField(payload, "skill");
                            var vrReqId = ExtractJsonField(json, "id");
                            if (string.IsNullOrEmpty(vrReqId)) vrReqId = Guid.NewGuid().ToString("N");
                            Log($"[VrSkills] skill={skill}");
                            var vrResult = VrSkills.ApplySkill(skill, payload);
                            var agent = FindFirstObjectByType<AoiNativeAgent>();
                            if (agent != null) agent.SendVrSkillResult(vrReqId, vrResult);
                        }
                        break;
                    case "awareness_on":                        Log("[AWARENESS] capture on");
                        envActive = true;
                        StartContinuousCapture();
                        {
                            var eui = GetPanelUI();
                            if (eui != null) eui.SetEnvActive(true);
                        }
                        UpdateHint();
                        break;
                    case "awareness_off":                        Log("[AWARENESS] capture off");
                        envActive = false;
                        StopContinuousCapture();
                        {
                            var eui2 = GetPanelUI();
                            if (eui2 != null) eui2.SetEnvActive(false);
                        }
                        UpdateHint();
                        break;
                }
            }
            catch (Exception e)            {
                Debug.LogError($"[Aoi] Agent msg: {e.Message}");
            }
        }
);
    }
    // Heuristic: does this assistant text look like an error the agent produced
    // (missing key, network failure, processing failure)? Rendered in red so the
    // user can tell a real reply from a failure.
    bool IsErrorText(string text)
    {
        if (string.IsNullOrEmpty(text)) return false;
        return text.Contains("缺少 API Key") ||
               text.Contains("网络异常") ||
               text.Contains("（处理失败") ||
               text.Contains("network error") ||
               text.Contains("HTTP error") ||
               text.StartsWith("Error", System.StringComparison.OrdinalIgnoreCase) ||
               text.StartsWith("错误", System.StringComparison.Ordinal);
    }

    // Extract a field's value from a JSON document, handling nested objects,
    // arrays and escape sequences correctly.
    //  - string fields -> returned WITHOUT surrounding quotes and WITHOUT the
    //    JSON unescape layer (i.e. already decoded, so `\n` -> newline).
    //  - numbers/bools  -> returned as their raw JSON text.
    //  - objects/arrays -> returned as their raw JSON text.
    // Returns "" if not found.
    string ExtractJsonField(string json, string field)    {
        try        {
            using var doc = System.Text.Json.JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != System.Text.Json.JsonValueKind.Object) return "";
            if (doc.RootElement.TryGetProperty(field, out var el))            {
                switch (el.ValueKind)                {
                    case System.Text.Json.JsonValueKind.String:
                        return el.GetString() ?? "";
                    case System.Text.Json.JsonValueKind.Null:
                        return "";
                    default:
                        return el.GetRawText();
                }
            }
        }
        catch        {
            // fall through to legacy parser for robustness
        }
        return ExtractJsonFieldLegacy(json, field);
    }
    string ExtractJsonFieldLegacy(string json, string field)    {
        var search = $"\"{field}\":";
        var start = json.IndexOf(search);
        if (start < 0) return "";
        start += search.Length;
        while (start < json.Length && char.IsWhiteSpace(json[start])) start++;
        if (start >= json.Length) return "";
        if (json[start] == '{' || json[start] == '[')        {
            var depth = 0;
            var end = start;
            while (end < json.Length)            {
                var ch = json[end];
                if (ch == '"')                {
                    end = SkipJsonString(json, end);
                    continue;
                }
                if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']')            {
                    depth--;
                    if (depth <= 0) { end++; break; }
                }
                end++;
            }
            return json.Substring(start, end - start);
        }
        if (json[start] == '"')        {
            var end = SkipJsonString(json, start);
            return UnescapeJsonString(json.Substring(start + 1, end - start - 1));
        }
        var idx = start;
        while (idx < json.Length && (char.IsLetterOrDigit(json[idx]) || json[idx] == '.' || json[idx] == '-' || json[idx] == '+' || json[idx] == 'e' || json[idx] == 'E')) idx++;
        return json.Substring(start, idx - start);
    }
    // Given the index of an opening quote, return the index of the closing quote
    // (un-escaped), treating \\ and \" correctly.
    int SkipJsonString(string s, int openIdx)    {
        var i = openIdx + 1;
        while (i < s.Length)        {
            if (s[i] == '\\') i += 2;
            else if (s[i] == '"') return i;
            else i++;
        }
        return s.Length;
    }
    // Decode JSON string escapes (\n \t \\ \" \uXXXX etc.) so the panel shows
    // the actual text instead of the escaped literal.
    string UnescapeJsonString(string s)    {
        if (string.IsNullOrEmpty(s) || s.IndexOf('\\') < 0) return s;
        var sb = new System.Text.StringBuilder(s.Length);
        for (int i = 0; i < s.Length; i++)        {
            var ch = s[i];
            if (ch != '\\' || i + 1 >= s.Length) { sb.Append(ch); continue; }
            var nx = s[i + 1];
            switch (nx)            {
                case '\\': sb.Append('\\'); i++; break;
                case '"': sb.Append('"'); i++; break;
                case '/': sb.Append('/'); i++; break;
                case 'b': sb.Append('\b'); i++; break;
                case 'f': sb.Append('\f'); i++; break;
                case 'n': sb.Append('\n'); i++; break;
                case 'r': sb.Append('\r'); i++; break;
                case 't': sb.Append('\t'); i++; break;
                case 'u':                {
                    if (i + 5 < s.Length && ushort.TryParse(s.Substring(i + 2, 4), System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out var code))                    {
                        sb.Append((char)code);
                        i += 5;
                    }
                    else { sb.Append('u'); i++; }
                    break;
                }
                default: sb.Append(nx); i++; break;
            }
        }
        return sb.ToString();
    }
    public void EnqueueMainThreadAction(Action action)    {
        mainThreadActions.Enqueue(action);
    }
    void OnDestroy() {
 Shutdown();
 }
    void OnApplicationQuit() {
 Shutdown();
 }
    void Shutdown()    {
        if (shutdownDone) return;
        shutdownDone = true;
        Log("Shutdown");
        StopAllCoroutines();
        DestroyLaserOverlay();
        if (handOverlayHandle != 0)        {
            var dErr = OpenVR.Overlay.DestroyOverlay(handOverlayHandle);
            Log($"DestroyOverlay(hand={handOverlayHandle}): {dErr}");
            handOverlayHandle = 0;
        }
        if (dashboardOverlayHandle != 0)        {
            var dErrDash = OpenVR.Overlay.DestroyOverlay(dashboardOverlayHandle);
            Log($"DestroyOverlay(dashboard={dashboardOverlayHandle}): {dErrDash}");
            dashboardOverlayHandle = 0;
        }
        if (dashboardThumbHandle != 0)        {
            OpenVR.Overlay.DestroyOverlay(dashboardThumbHandle);
            dashboardThumbHandle = 0;
        }
        ulong leftover = 0;
        var findErr2 = OpenVR.Overlay.FindOverlay(overlayKey, ref leftover);
        if (findErr2 == EVROverlayError.None && leftover != 0)        {
            var dErr2 = OpenVR.Overlay.DestroyOverlay(leftover);
            Log($"Shutdown cleanup stale overlay {leftover}: {dErr2}");
        }
        if (cachedMirrorSrv != IntPtr.Zero && OpenVR.Compositor != null)        {
            OpenVR.Compositor.ReleaseMirrorTextureD3D11(cachedMirrorSrv);
            cachedMirrorSrv = IntPtr.Zero;
        }
        if (openVRReady)        {
            OpenVR.Shutdown();
            openVRReady = false;
        }
        if (renderTexture != null)        {
            renderTexture.Release();
            Destroy(renderTexture);
            renderTexture = null;
        }
        if (captureTex2D != null)        {
            Destroy(captureTex2D);
            captureTex2D = null;
        }
        Log("Shutdown complete");
        if (singleInstanceMutexHandle != IntPtr.Zero)        {
            try {
                CloseHandle(singleInstanceMutexHandle);
            }
            catch {
            }
            singleInstanceMutexHandle = IntPtr.Zero;
        }
        logWriter?.Close();
        logWriter = null;
    }
}
