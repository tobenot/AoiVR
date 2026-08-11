using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;
using TMPro;

// Aoi hand panel UI. Everything is authored in the mockup's own coordinate space
// (684x684, see mockup/jarvis-panel.html). Resolution independence is handled by
// CanvasScaler (referenceResolution = 684) and the VR world-space canvas in
// AoiOrchestrator (sizeDelta 684, localScale 1/684), so every layout number
// below is a raw mockup pixel value and relative proportions hold at any resolution.
public class AoiBootstrap : MonoBehaviour
{
    public const float Board = 684f; // mockup design space (square); the 684 render is the intended design

    // Palette (from mockup computed styles)
    static readonly Color BgVoid = new Color(0.020f, 0.027f, 0.051f, 1f);          // rgb(5,7,13)
    static readonly Color ChatFill = new Color(0.031f, 0.055f, 0.102f, 0.82f);     // rgba(8,14,26,.82)
    static readonly Color Cyan = new Color(0.008f, 0.843f, 0.949f);                // rgb(2,215,242)
    static readonly Color CyanDim = new Color(0.008f, 0.843f, 0.949f, 0.55f);
    static readonly Color CyanSoft = new Color(0.008f, 0.843f, 0.949f, 0.25f);
    static readonly Color Yellow = new Color(0.953f, 0.902f, 0f);                  // #F3E600
    static readonly Color Amber = new Color(1f, 0.843f, 0.369f);                   // rgb(255,215,94)
    static readonly Color Purple = new Color(0.773f, 0.549f, 1f);                  // rgb(197,140,255)
    static readonly Color PurpleBorder = new Color(0.69f, 0f, 1f, 0.55f);          // rgba(176,0,255,.55)
    static readonly Color TextMain = new Color(0.843f, 0.953f, 0.984f);            // rgb(215,243,251)
    static readonly Color TextDim = new Color(0.498f, 0.659f, 0.722f);             // rgb(127,168,184)
    static readonly Color SubFill = new Color(0.016f, 0.024f, 0.055f, 0.9f);       // rgba(4,6,14,.9)
    static readonly Color ToastFill = new Color(0.016f, 0.024f, 0.055f, 0.92f);    // rgba(4,6,14,.92)

    void Awake()
    {
        var orchestrator = gameObject.AddComponent<AoiOrchestrator>();

        // Native C++ agent (aoi_agent.dll). Runs on its own background thread;
        // talks to Unity in-process via C ABI (SendJson + callback). No pipe.
        var nativeAgent = gameObject.AddComponent<AoiNativeAgent>();

        var canvasGO = new GameObject("HandPanelCanvas");
        canvasGO.transform.SetParent(transform);
        var canvas = canvasGO.AddComponent<Canvas>();
        canvas.renderMode = RenderMode.ScreenSpaceCamera;
        canvas.worldCamera = orchestrator.uiCamera;
        canvas.planeDistance = 0.5f;

        var scaler = canvasGO.AddComponent<CanvasScaler>();
        scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
        scaler.referenceResolution = new Vector2(Board, Board);
        scaler.screenMatchMode = CanvasScaler.ScreenMatchMode.MatchWidthOrHeight;
        scaler.matchWidthOrHeight = 0.5f;

        canvasGO.AddComponent<GraphicRaycaster>();

        CreateSkin(canvasGO.transform);

        var statusBar = CreateStatusBar(canvasGO.transform,
            out var logoText, out var logoVerText, out var statusDot, out var statusText,
            out var interpChip, out var interpChipText, out var envChip, out var envChipText,
            out var pipelineContainer, out var procbar,
            out var pstepDots, out var pstepLabels, out var pstepRects, out var pstepConns,
            out var procbarText, out var procbarCanvasGroup);
        var chatArea = CreateChatArea(canvasGO.transform);
        var scrollUI = CreateScrollUI(canvasGO.transform, out var scrollTrack, out var scrollThumb,
            out var btnUp, out var btnDown);
        var subtitleBar = CreateSubtitleBar(canvasGO.transform,
            out var subtitleText, out var subtitleTagText);
        var hintBar = CreateHintBar(canvasGO.transform, out var hintText);
        var shotToast = CreateShotToast(canvasGO.transform,
            out var shotToastTitle, out var shotToastSub);

        var panelUI = canvasGO.AddComponent<HandPanelUI>();
        panelUI.logoText = logoText;
        panelUI.logoVerText = logoVerText;
        panelUI.statusText = statusText;
        panelUI.statusDot = statusDot;
        panelUI.interpChip = interpChip;
        panelUI.interpChipText = interpChipText;
        panelUI.envChip = envChip;
        panelUI.envChipText = envChipText;
        panelUI.pipelineContainer = pipelineContainer;
        panelUI.pstepDots = pstepDots;
        panelUI.pstepLabels = pstepLabels;
        panelUI.pstepRects = pstepRects;
        panelUI.pstepConns = pstepConns;
        panelUI.subtitleBar = subtitleBar;
        panelUI.subtitleText = subtitleText;
        panelUI.subtitleTagText = subtitleTagText;
        panelUI.hintText = hintText;
        panelUI.shotToast = shotToast;
        panelUI.shotToastTitle = shotToastTitle;
        panelUI.shotToastSub = shotToastSub;

        TextMeshProUGUI chatText = null;
        foreach (var t in canvasGO.GetComponentsInChildren<TextMeshProUGUI>(true))
        {
            if (t.name == "ChatText") { chatText = t; break; }
        }
        panelUI.chatText = chatText;
        panelUI.chatScroll = chatArea.GetComponentInChildren<ScrollRect>(true);
        panelUI.chatScrollUp = btnUp;
        panelUI.chatScrollDown = btnDown;
        panelUI.scrollTrack = scrollTrack;
        panelUI.scrollThumb = scrollThumb;

        var setupEventSystem = new GameObject("EventSystem");
        setupEventSystem.transform.SetParent(transform);
        var es = setupEventSystem.AddComponent<EventSystem>();
        var steamInput = setupEventSystem.AddComponent<SteamVROverlayInput>();
        var inputModule = setupEventSystem.AddComponent<StandaloneInputModule>();
        inputModule.inputOverride = steamInput;
        es.pixelDragThreshold = 100000;
        es.firstSelectedGameObject = null;
        // No keyboard navigation: arrow/PageUp/PageDown keys would select the
        // ▲▼ scroll buttons and trigger the hover highlight color change.
        es.sendNavigationEvents = false;

        // procbar LAST so it renders on top of everything (chat, toasts, etc).
        var procbarGO = CreateProcbar(canvasGO.transform, out var procTextGO, out var procbarCG);
        panelUI.procbarText = procTextGO;
        panelUI.procbarCanvasGroup = procbarCG;

        SetLayerRecursive(canvasGO.transform, LayerMask.NameToLayer("UI"));
    }

    void SetLayerRecursive(Transform t, int layer)
    {
        t.gameObject.layer = layer;
        foreach (Transform c in t)
            SetLayerRecursive(c, layer);
    }

    // ---------- skin: panel frame (cut 26), grid (48px), scanlines, corner brackets ----------
    void CreateSkin(Transform parent)
    {
        // panel frame carries the void background + border + corner brackets; the
        // cut octagon makes the corner triangles transparent (alpha 0).
        var frame = new GameObject("PanelFrame");
        frame.transform.SetParent(parent, false);
        var frameRT = frame.AddComponent<RectTransform>();
        frameRT.anchorMin = Vector2.zero; frameRT.anchorMax = Vector2.one;
        frameRT.offsetMin = Vector2.zero; frameRT.offsetMax = Vector2.zero;
        var frameImg = frame.AddComponent<Image>();
        frameImg.sprite = MakeCutFrameWithBrackets(Board, Board, 26, 1, CyanDim, BgVoid,
            15, 34, 2, Cyan);
        frameImg.type = Image.Type.Simple;
        frameImg.preserveAspect = false;
        frameImg.raycastTarget = false;

        // full-board grid (48px cells, line alpha 0.043), clipped to the cut octagon
        var gridGO = new GameObject("BgGrid");
        gridGO.transform.SetParent(parent, false);
        var gridRT = gridGO.AddComponent<RectTransform>();
        gridRT.anchorMin = Vector2.zero; gridRT.anchorMax = Vector2.one;
        gridRT.offsetMin = Vector2.zero; gridRT.offsetMax = Vector2.zero;
        var gridImg = gridGO.AddComponent<Image>();
        gridImg.sprite = MakeCanvasGridTexture(48, new Color32(2, 215, 242, 11), 26);
        gridImg.type = Image.Type.Simple;
        gridImg.preserveAspect = false;
        gridImg.raycastTarget = false;

        // full-board scanlines (1px white 0.024 every 3px), clipped to the cut octagon
        var scanGO = new GameObject("BgScanlines");
        scanGO.transform.SetParent(parent, false);
        var scanRT = scanGO.AddComponent<RectTransform>();
        scanRT.anchorMin = Vector2.zero; scanRT.anchorMax = Vector2.one;
        scanRT.offsetMin = Vector2.zero; scanRT.offsetMax = Vector2.zero;
        var scanImg = scanGO.AddComponent<Image>();
        scanImg.sprite = MakeCanvasScanTexture(3, new Color32(255, 255, 255, 6), 26);
        scanImg.type = Image.Type.Simple;
        scanImg.preserveAspect = false;
        scanImg.raycastTarget = false;
    }

    void CreateCornerBracket(Transform parent, string name, Vector2 aMin, Vector2 aMax, Vector2 pos, bool left, bool top)
    {
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        var goRT = go.AddComponent<RectTransform>();
        goRT.anchorMin = Vector2.zero; goRT.anchorMax = Vector2.one;
        goRT.anchoredPosition = Vector2.zero; goRT.sizeDelta = Vector2.zero;
        var h = new GameObject("H");
        h.transform.SetParent(go.transform, false);
        var hrt = h.AddComponent<RectTransform>();
        hrt.anchorMin = aMin; hrt.anchorMax = aMax;
        hrt.pivot = new Vector2(left ? 0 : 1, top ? 1 : 0);
        hrt.anchoredPosition = pos;
        hrt.sizeDelta = new Vector2(34, 2);
        var himg = h.AddComponent<Image>();
        himg.color = Cyan;
        himg.raycastTarget = false;
        var v = new GameObject("V");
        v.transform.SetParent(go.transform, false);
        var vrt = v.AddComponent<RectTransform>();
        vrt.anchorMin = aMin; vrt.anchorMax = aMax;
        vrt.pivot = new Vector2(left ? 0 : 1, top ? 1 : 0);
        vrt.anchoredPosition = pos;
        vrt.sizeDelta = new Vector2(2, 34);
        var vimg = v.AddComponent<Image>();
        vimg.color = Cyan;
        vimg.raycastTarget = false;
    }

    // Full-board grid texture: vertical + horizontal lines every `cell` px,
    // clipped to the cut octagon so corner triangles stay transparent.
    Sprite MakeCanvasGridTexture(int cell, Color32 line, int cut)
    {
        int s = Mathf.RoundToInt(Board);
        var tex = new Texture2D(s, s, TextureFormat.RGBA32, false);
        var px = new Color32[s * s];
        var clear = new Color32(0, 0, 0, 0);
        for (int y = 0; y < s; y++)
        {
            bool hline = (y % cell) == 0;
            for (int x = 0; x < s; x++)
            {
                if (!InCut(x, y, s, s, cut)) { px[y * s + x] = clear; continue; }
                px[y * s + x] = (hline || (x % cell) == 0) ? line : clear;
            }
        }
        tex.SetPixels32(px);
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, s, s), new Vector2(0.5f, 0.5f), 1f);
    }

    // Full-board scanline texture: a 1px line every `period` px vertically,
    // clipped to the cut octagon so corner triangles stay transparent.
    Sprite MakeCanvasScanTexture(int period, Color32 line, int cut)
    {
        int s = Mathf.RoundToInt(Board);
        var tex = new Texture2D(s, s, TextureFormat.RGBA32, false);
        var px = new Color32[s * s];
        var clear = new Color32(0, 0, 0, 0);
        for (int y = 0; y < s; y++)
        {
            bool on = (y % period) == 0;
            for (int x = 0; x < s; x++)
            {
                if (!InCut(x, y, s, s, cut)) { px[y * s + x] = clear; continue; }
                px[y * s + x] = on ? line : clear;
            }
        }
        tex.SetPixels32(px);
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, s, s), new Vector2(0.5f, 0.5f), 1f);
    }

    // Cut-corner octagon frame with the corner L-brackets baked in. The brackets
    // are clipped by the octagon, so their outer tips are cut off exactly like the
    // mockup's clip-path does (bracket hugs the cut corner instead of spanning it).
    Sprite MakeCutFrameWithBrackets(float wU, float hU, float cutU, float borderU,
        Color borderColor, Color fillColor,
        float bracketInset, float bracketLen, float bracketThick, Color bracketColor)
    {
        int w = Mathf.Max(4, Mathf.RoundToInt(wU * 2f));
        int h = Mathf.Max(4, Mathf.RoundToInt(hU * 2f));
        int cut = Mathf.RoundToInt(cutU * 2f);
        int bp = Mathf.Max(1, Mathf.RoundToInt(borderU * 2f));
        var tex = new Texture2D(w, h, TextureFormat.RGBA32, false);
        var px = new Color32[w * h];
        var bc = (Color32)borderColor;
        var fc = (Color32)fillColor;
        var kc = (Color32)bracketColor;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                if (!InCut(x, y, w, h, cut)) { px[y * w + x] = new Color32(0, 0, 0, 0); continue; }
                bool border = !ErodedInCut(x, y, w, h, cut, bp);
                px[y * w + x] = border ? bc : fc;
            }
        }
        int inset = Mathf.RoundToInt(bracketInset * 2f);
        int len = Mathf.RoundToInt(bracketLen * 2f);
        int thick = Mathf.Max(1, Mathf.RoundToInt(bracketThick * 2f));
        DrawBracket(px, w, h, cut, inset, inset, len, thick, 1, 1, kc);            // TL
        DrawBracket(px, w, h, cut, w - inset, inset, len, thick, -1, 1, kc);       // TR
        DrawBracket(px, w, h, cut, inset, h - inset, len, thick, 1, -1, kc);       // BL
        DrawBracket(px, w, h, cut, w - inset, h - inset, len, thick, -1, -1, kc);  // BR
        tex.SetPixels32(px);
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, w, h), new Vector2(0.5f, 0.5f), 2f);
    }

    void DrawBracket(Color32[] px, int w, int h, int cut, int cx, int cy, int len, int thick, int dx, int dy, Color32 color)
    {
        // horizontal arm
        for (int i = 0; i < len; i++)
        {
            int ax = cx + dx * i;
            for (int t = 0; t < thick; t++)
            {
                int ay = cy + dy * t;
                if (ax >= 0 && ax < w && ay >= 0 && ay < h && InCut(ax, ay, w, h, cut))
                    px[ay * w + ax] = color;
            }
        }
        // vertical arm
        for (int j = 0; j < len; j++)
        {
            int ay = cy + dy * j;
            for (int t = 0; t < thick; t++)
            {
                int ax = cx + dx * t;
                if (ax >= 0 && ax < w && ay >= 0 && ay < h && InCut(ax, ay, w, h, cut))
                    px[ay * w + ax] = color;
            }
        }
    }

    // Exact-size cut-corner octagon panel sprite (2x resolution for crisp downscale).
    // Sizes are in canvas units (mockup px).
    Sprite MakeCutPanel(float wU, float hU, float cutU, float borderU, Color borderColor, Color fillColor)
    {
        int w = Mathf.Max(4, Mathf.RoundToInt(wU * 2f));
        int h = Mathf.Max(4, Mathf.RoundToInt(hU * 2f));
        int cut = Mathf.RoundToInt(cutU * 2f);
        int bp = Mathf.Max(1, Mathf.RoundToInt(borderU * 2f));
        var tex = new Texture2D(w, h, TextureFormat.RGBA32, false);
        var px = new Color32[w * h];
        var bc = (Color32)borderColor;
        var fc = (Color32)fillColor;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                if (!InCut(x, y, w, h, cut)) { px[y * w + x] = new Color32(0, 0, 0, 0); continue; }
                bool border = !ErodedInCut(x, y, w, h, cut, bp);
                px[y * w + x] = border ? bc : fc;
            }
        }
        tex.SetPixels32(px);
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, w, h), new Vector2(0.5f, 0.5f), 2f);
    }

    bool InCut(int x, int y, int w, int h, int cut)
    {
        if (x < cut && y < cut && (cut - x) + (cut - y) > cut) return false;
        if (x >= w - cut && y < cut && (x - (w - cut - 1)) + (cut - y) > cut) return false;
        if (x < cut && y >= h - cut && (cut - x) + (y - (h - cut - 1)) > cut) return false;
        if (x >= w - cut && y >= h - cut && (x - (w - cut - 1)) + (y - (h - cut - 1)) > cut) return false;
        return true;
    }

    bool ErodedInCut(int x, int y, int w, int h, int cut, int erode)
    {
        int w2 = w - 2 * erode, h2 = h - 2 * erode;
        int x2 = x - erode, y2 = y - erode;
        if (x2 < 0 || y2 < 0 || x2 >= w2 || y2 >= h2) return false;
        return InCut(x2, y2, w2, h2, cut);
    }

    // Panel with only top-left and bottom-right corners cut (mockup procbar shape:
    // square with two opposite corners clipped, not an octagon). Full square
    // border, only the two corner triangles are empty.
    public static Sprite MakeCutPanel2Corners(float wU, float hU, float cutU, float borderU, Color borderColor, Color fillColor)
    {
        int w = Mathf.Max(4, Mathf.RoundToInt(wU * 2f));
        int h = Mathf.Max(4, Mathf.RoundToInt(hU * 2f));
        int cut = Mathf.RoundToInt(cutU * 2f);
        int bp = Mathf.Max(1, Mathf.RoundToInt(borderU * 2f));
        var tex = new Texture2D(w, h, TextureFormat.RGBA32, false);
        var px = new Color32[w * h];
        var bc = (Color32)borderColor;
        var fc = (Color32)fillColor;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                // top-left cut triangle: x<cut && y<cut && (cut-x)+(cut-y)>cut
                bool tlCut = x < cut && y < cut && (cut - x) + (cut - y) > cut;
                // bottom-right cut triangle: x>=w-cut && y>=h-cut && (x-(w-cut-1))+(y-(h-cut-1))>cut
                bool brCut = x >= w - cut && y >= h - cut && (x - (w - cut - 1)) + (y - (h - cut - 1)) > cut;
                if (tlCut || brCut) { px[y * w + x] = new Color32(0, 0, 0, 0); continue; }
                // border: 1px frame along the outer edge (all four sides)
                bool border = x < bp || x >= w - bp || y < bp || y >= h - bp;
                px[y * w + x] = border ? bc : fc;
            }
        }
        tex.SetPixels32(px);
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, w, h), new Vector2(0.5f, 0.5f), 2f);
    }

    // Soft disc with a tight halo (quadratic decay). White with alpha; tint via Image.color.
    Sprite MakeCircleSprite(float discRadius, float glowRadius)
    {
        int size = Mathf.CeilToInt((discRadius + glowRadius) * 2f);
        var tex = new Texture2D(size, size, TextureFormat.RGBA32, false);
        var px = new Color32[size * size];
        float c = (size - 1) / 2f;
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                float d = Mathf.Sqrt((x - c) * (x - c) + (y - c) * (y - c));
                float a;
                if (d <= discRadius) a = 1f;
                else if (d >= discRadius + glowRadius) a = 0f;
                else
                {
                    float t = (d - discRadius) / glowRadius;
                    a = (1f - t) * (1f - t);
                }
                px[y * size + x] = new Color32(255, 255, 255, (byte)(a * 255f));
            }
        }
        tex.SetPixels32(px);
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, size, size), new Vector2(0.5f, 0.5f), 100f);
    }

    // Horizontal bar sprite: solidWidth solid center fading to the edges; stretched
    // vertically it reads as a glowing scrollbar thumb.
    Sprite MakeBarGlowSprite(int width, int solidWidth, Color color)
    {
        var tex = new Texture2D(width, 1, TextureFormat.RGBA32, false);
        float c = (width - 1) / 2f;
        float half = solidWidth / 2f;
        float fallRange = width / 2f - half;
        var bc = (Color32)color;
        for (int x = 0; x < width; x++)
        {
            float d = Mathf.Abs(x - c);
            float a;
            if (d <= half) a = 1f;
            else
            {
                float t = (d - half) / fallRange;
                a = (1f - t) * (1f - t);
            }
            var col = bc;
            col.a = (byte)(bc.a * a);
            tex.SetPixel(x, 0, col);
        }
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, width, 1), new Vector2(0.5f, 0.5f), 1f);
    }

    Sprite MakeGradientSprite(Color top, int h)
    {
        var tex = new Texture2D(1, h, TextureFormat.RGBA32, false);
        var tc = (Color32)top;
        for (int y = 0; y < h; y++)
        {
            float t = (float)y / (h - 1);
            var c = tc;
            c.a = (byte)(tc.a * t);
            tex.SetPixel(0, y, c);
        }
        tex.Apply();
        tex.filterMode = FilterMode.Bilinear;
        return Sprite.Create(tex, new Rect(0, 0, 1, h), new Vector2(0.5f, 0.5f), 1f);
    }

    // 1px border made of 4 line rects at the parent's edges (for variable-size
    // panels whose interior must stay truly transparent, e.g. chips).
    void CreateBorderLines(Transform parent, Color color)
    {
        MakeLine(parent, "BTop", new Vector2(0, 1), new Vector2(1, 1), new Vector2(0, -1), new Vector2(0, 1), color);
        MakeLine(parent, "BBottom", new Vector2(0, 0), new Vector2(1, 0), Vector2.zero, new Vector2(0, 1), color);
        MakeLine(parent, "BLeft", new Vector2(0, 0), new Vector2(0, 1), Vector2.zero, new Vector2(1, 0), color);
        MakeLine(parent, "BRight", new Vector2(1, 0), new Vector2(1, 1), new Vector2(-1, 0), new Vector2(1, 0), color);
    }

    void MakeLine(Transform parent, string name, Vector2 aMin, Vector2 aMax, Vector2 oMin, Vector2 oMax, Color color)
    {
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = aMin; rt.anchorMax = aMax;
        rt.offsetMin = oMin; rt.offsetMax = oMax;
        var img = go.AddComponent<Image>();
        img.color = color;
        img.raycastTarget = false;
    }

    // Processing stage label (procbar), created LAST so it renders on top of
    // everything else (chat, toasts, etc). Ephemeral, replaced in place.
    // Shape: square with only top-left & bottom-right corners cut (not octagon).
    // Width adapts to the stage text length (text + padding).
    GameObject CreateProcbar(Transform parent,
        out TextMeshProUGUI procText, out CanvasGroup procbarCanvasGroup)
    {
        var procbar = new GameObject("Procbar");
        procbar.transform.SetParent(parent, false);
        var pbRT = procbar.AddComponent<RectTransform>();
        pbRT.anchorMin = new Vector2(0, 1);
        pbRT.anchorMax = new Vector2(0, 1);
        pbRT.pivot = new Vector2(0, 1);
        pbRT.anchoredPosition = new Vector2(34, -76);
        pbRT.sizeDelta = new Vector2(300, 34);
        var pbImg = procbar.AddComponent<Image>();
        pbImg.sprite = MakeCutPanel2Corners(300, 34, 8, 1,
            new Color(0.953f, 0.902f, 0f, 0.4f),
            new Color(0.016f, 0.024f, 0.055f, 0.9f));
        pbImg.type = Image.Type.Simple;
        pbImg.preserveAspect = false;
        pbImg.raycastTarget = false;
        var pbCG = procbar.AddComponent<CanvasGroup>();
        pbCG.alpha = 0f;
        pbCG.interactable = false;
        pbCG.blocksRaycasts = false;
        procText = CreateText(procbar.transform, "Text", "", 12.5f,
            TextAlignmentOptions.MidlineLeft, Color.yellow, false);
        procText.font = AoiOrchestrator.ResolveMonoFont();
        procText.characterSpacing = 10f;
        // Mask (not Ellipsis): the procbar text scrolls horizontally like a
        // marquee when the thought stream is longer than the panel, so new
        // content stays visible. Requires a RectMask2D on the panel.
        procText.overflowMode = TextOverflowModes.Masking;
        procText.enableWordWrapping = false;  // single line; marquee scrolls horizontally
        procbar.AddComponent<RectMask2D>();
        var ptRT = procText.GetComponent<RectTransform>();
        ptRT.anchorMin = Vector2.zero;
        ptRT.anchorMax = Vector2.one;
        ptRT.offsetMin = new Vector2(12, 0);
        ptRT.offsetMax = new Vector2(-12, 0);
        procbarCanvasGroup = pbCG;
        return procbar;
    }

    // ---------- status bar (64 tall, gradient tint + bottom 1px cyan-soft) ----------
    GameObject CreateStatusBar(Transform parent,
        out TextMeshProUGUI logoText, out TextMeshProUGUI logoVerText,
        out Image statusDot, out TextMeshProUGUI statusText,
        out GameObject interpChip, out TextMeshProUGUI interpChipText,
        out GameObject envChip, out TextMeshProUGUI envChipText,
        out GameObject outPipeline, out GameObject outProcbar,
        out Image[] outPstepDots, out TextMeshProUGUI[] outPstepLabels,
        out RectTransform[] outPstepRects, out TextMeshProUGUI[] outPstepConns,
        out TextMeshProUGUI outProcbarText, out CanvasGroup outProcbarCanvasGroup)
    {
        var bar = CreateRect(parent, "StatusBar", new Color(0, 0, 0, 0),
            new Vector2(0, 1), new Vector2(1, 1), new Vector2(0, -32), new Vector2(0, 64));
        var grad = new GameObject("Gradient");
        grad.transform.SetParent(bar.transform, false);
        var gradRT = grad.AddComponent<RectTransform>();
        gradRT.anchorMin = Vector2.zero; gradRT.anchorMax = Vector2.one;
        gradRT.offsetMin = Vector2.zero; gradRT.offsetMax = Vector2.zero;
        var gradImg = grad.AddComponent<Image>();
        gradImg.sprite = MakeGradientSprite(new Color(0.008f, 0.843f, 0.949f, 0.07f), 64);
        gradImg.type = Image.Type.Simple;
        gradImg.preserveAspect = false;
        gradImg.raycastTarget = false;
        var div = new GameObject("Divider");
        div.transform.SetParent(bar.transform, false);
        var divRT = div.AddComponent<RectTransform>();
        divRT.anchorMin = new Vector2(0, 0); divRT.anchorMax = new Vector2(1, 0);
        divRT.offsetMin = Vector2.zero; divRT.offsetMax = new Vector2(0, 1);
        var divImg = div.AddComponent<Image>();
        divImg.color = CyanSoft;
        divImg.raycastTarget = false;

        // logo main: "Aoi", x=31, font 21 JetBrains Mono bold, A cyan / oi yellow
        logoText = CreateText(bar.transform, "Logo", "<b>A</b><b><color=#F3E600>oi</color></b>",
            21, TextAlignmentOptions.MidlineLeft, Cyan, true);
        logoText.fontStyle = FontStyles.Bold;
        logoText.characterSpacing = 14f;
        logoText.font = AoiOrchestrator.ResolveMonoFont();
        var logoRT = logoText.GetComponent<RectTransform>();
        logoRT.anchorMin = new Vector2(0, 0.5f); logoRT.anchorMax = new Vector2(0, 0.5f);
        logoRT.pivot = new Vector2(0, 0.5f);
        logoRT.anchoredPosition = new Vector2(31, 0);
        logoRT.sizeDelta = new Vector2(140, 26);

        // logo ver: "HAND PANEL" tiny 10, cyan 0.4, tracking 20%, after main + 8, raised 2
        logoVerText = CreateText(bar.transform, "LogoVer", "HAND PANEL",
            10, TextAlignmentOptions.MidlineLeft, new Color(0.008f, 0.843f, 0.949f, 0.4f), false);
        logoVerText.characterSpacing = 20f;
        logoVerText.font = AoiOrchestrator.ResolveMonoFont();
        var verRT = logoVerText.GetComponent<RectTransform>();
        verRT.anchorMin = new Vector2(0, 0.5f); verRT.anchorMax = new Vector2(0, 0.5f);
        verRT.pivot = new Vector2(0, 0.5f);
        verRT.anchoredPosition = new Vector2(31 + MeasureTextWidth(logoText) + 8, 2);
        verRT.sizeDelta = new Vector2(160, 16);

        // status dot: 9px circle with tight halo
        var dotGO = new GameObject("StatusDot");
        dotGO.transform.SetParent(bar.transform, false);
        var dotRT = dotGO.AddComponent<RectTransform>();
        dotRT.anchorMin = new Vector2(0, 0.5f); dotRT.anchorMax = new Vector2(0, 0.5f);
        dotRT.pivot = new Vector2(0.5f, 0.5f);
        dotRT.anchoredPosition = new Vector2(174, 0);
        dotRT.sizeDelta = new Vector2(19, 19);
        statusDot = dotGO.AddComponent<Image>();
        statusDot.sprite = MakeCircleSprite(4.5f, 5f);
        statusDot.color = Cyan;
        statusDot.raycastTarget = false;

        // status text: JetBrains Mono 15, tracking 14%, cyan
        statusText = CreateText(bar.transform, "StatusText", "● 就绪", 15, TextAlignmentOptions.MidlineLeft, Cyan, true);
        statusText.characterSpacing = 14f;
        statusText.font = AoiOrchestrator.ResolveMonoFont();
        var stRT = statusText.GetComponent<RectTransform>();
        stRT.anchorMin = new Vector2(0, 0.5f); stRT.anchorMax = new Vector2(0, 0.5f);
        stRT.pivot = new Vector2(0, 0.5f);
        stRT.anchoredPosition = new Vector2(187, 0);
        stRT.sizeDelta = new Vector2(220, 24);
        statusText.overflowMode = TextOverflowModes.Overflow;

        // chips (dynamic x via HandPanelUI.RelayoutStatusBar)
        interpChip = CreateChip(bar.transform, "InterpChip", "实时字幕 →中", Amber,
            new Color(1f, 0.843f, 0.369f, 0.55f), out interpChipText);
        interpChip.SetActive(false);
        envChip = CreateChip(bar.transform, "EnvChip", "环境上下文", Purple,
            PurpleBorder, out envChipText);
        envChip.SetActive(false);

        // Processing stage indicator: only the procbar label (above chat) is
        // used; the in-statusbar pstep dots were removed (they collided with
        // the env/interp chips).

        outPipeline = null;
        outProcbar = null;
        outPstepDots = new Image[3];
        outPstepLabels = new TextMeshProUGUI[3];
        outPstepRects = new RectTransform[3];
        outPstepConns = new TextMeshProUGUI[2];
        outProcbarText = null;
        outProcbarCanvasGroup = null;

        // binding button: right edge 31 from right, 88x29, font 12.5, dim, border only
        var bindBtn = new GameObject("BindingBtn");
        bindBtn.transform.SetParent(bar.transform, false);
        var bindRT = bindBtn.AddComponent<RectTransform>();
        bindRT.anchorMin = new Vector2(1, 0.5f); bindRT.anchorMax = new Vector2(1, 0.5f);
        bindRT.pivot = new Vector2(1, 0.5f);
        bindRT.anchoredPosition = new Vector2(-31, 0);
        bindRT.sizeDelta = new Vector2(88, 29);
        var bindImg = bindBtn.AddComponent<Image>();
        bindImg.sprite = MakeCutPanel(88, 29, 6, 1, CyanDim, new Color(0, 0, 0, 0));
        bindImg.type = Image.Type.Simple;
        bindImg.preserveAspect = false;
        var bindBtnC = bindBtn.AddComponent<Button>();
        bindBtnC.targetGraphic = bindImg;
        bindBtnC.transition = Selectable.Transition.None;
        var bindTxt = CreateText(bindBtn.transform, "Label", "修改绑定", 12.5f, TextAlignmentOptions.Center, TextDim, false);
        bindTxt.characterSpacing = 12f;
        bindTxt.font = AoiOrchestrator.ResolveMonoFont();
        var btRT = bindTxt.GetComponent<RectTransform>();
        btRT.anchorMin = Vector2.zero; btRT.anchorMax = Vector2.one;
        btRT.offsetMin = Vector2.zero; btRT.offsetMax = Vector2.zero;
        var bindHover = bindBtn.AddComponent<UiHoverColor>();
        bindHover.hoverColor = Cyan;
        bindHover.graphics = new Graphic[] { bindImg };
        bindHover.texts = new TextMeshProUGUI[] { bindTxt };
        bindBtnC.onClick.AddListener(() =>
        {
            var orch = FindObjectOfType<AoiOrchestrator>();
            if (orch != null) orch.OpenBindingSettings();
        });

        return bar;
    }

    // chip: transparent interior + 1px line border, circle dot at x=15.5, text at x=26
    GameObject CreateChip(Transform parent, string name, string label, Color textColor,
        Color borderColor, out TextMeshProUGUI chipText)
    {
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = new Vector2(0, 0.5f); rt.anchorMax = new Vector2(0, 0.5f);
        rt.pivot = new Vector2(0, 0.5f);
        rt.anchoredPosition = new Vector2(256, 0);
        rt.sizeDelta = new Vector2(120, 24);
        CreateBorderLines(go.transform, borderColor);

        // 7px circle dot + tight halo, center at x=15.5
        var dot = new GameObject("Dot");
        dot.transform.SetParent(go.transform, false);
        var dotRT = dot.AddComponent<RectTransform>();
        dotRT.anchorMin = new Vector2(0, 0.5f); dotRT.anchorMax = new Vector2(0, 0.5f);
        dotRT.pivot = new Vector2(0.5f, 0.5f);
        dotRT.anchoredPosition = new Vector2(15.5f, 0);
        dotRT.sizeDelta = new Vector2(15, 15);
        var dotImg = dot.AddComponent<Image>();
        dotImg.sprite = MakeCircleSprite(3.5f, 4f);
        dotImg.color = textColor;
        dotImg.raycastTarget = false;

        chipText = CreateText(go.transform, "Label", label, 12, TextAlignmentOptions.MidlineLeft, textColor, false);
        chipText.characterSpacing = 14f;
        chipText.font = AoiOrchestrator.ResolveMonoFont();
        var txtRT = chipText.GetComponent<RectTransform>();
        txtRT.anchorMin = new Vector2(0, 0.5f); txtRT.anchorMax = new Vector2(1, 0.5f);
        txtRT.offsetMin = new Vector2(26, -12);
        txtRT.offsetMax = new Vector2(-4, 12);
        return go;
    }

    float MeasureTextWidth(TextMeshProUGUI t)
    {
        if (t == null) return 0f;
        t.ForceMeshUpdate();
        return t.preferredWidth;
    }

    // ---------- chat area (left 34, top 92, 616x422; cut 16, cyan-soft border) ----------
    GameObject CreateChatArea(Transform parent)
    {
        var page = new GameObject("ChatArea");
        page.transform.SetParent(parent, false);
        var pageRT = page.AddComponent<RectTransform>();
        pageRT.anchorMin = new Vector2(0.5f, 0.5f);
        pageRT.anchorMax = new Vector2(0.5f, 0.5f);
        pageRT.anchoredPosition = new Vector2(0, Board / 2f - (92f + (Board - 92f - 170f) / 2f));
        pageRT.sizeDelta = new Vector2(Board - 68f, Board - 92f - 170f);
        var pageImg = page.AddComponent<Image>();
        pageImg.sprite = MakeCutPanel(Board - 68f, Board - 92f - 170f, 16, 1, CyanSoft, ChatFill);
        pageImg.type = Image.Type.Simple;
        pageImg.preserveAspect = false;
        pageImg.raycastTarget = true;

        var scrollGO = new GameObject("ChatScroll");
        scrollGO.transform.SetParent(page.transform, false);
        var scrollRT = scrollGO.AddComponent<RectTransform>();
        scrollRT.anchorMin = Vector2.zero;
        scrollRT.anchorMax = Vector2.one;
        scrollRT.offsetMin = new Vector2(23, 19);
        scrollRT.offsetMax = new Vector2(-65, -19);
        scrollGO.AddComponent<RectMask2D>();
        var scrollRect = scrollGO.AddComponent<ScrollRect>();
        scrollRect.horizontal = false;
        scrollRect.vertical = true;
        scrollRect.movementType = ScrollRect.MovementType.Elastic;
        scrollRect.scrollSensitivity = 40;

        var viewport = new GameObject("Viewport");
        viewport.transform.SetParent(scrollGO.transform, false);
        var vpRT = viewport.AddComponent<RectTransform>();
        vpRT.anchorMin = Vector2.zero;
        vpRT.anchorMax = Vector2.one;
        vpRT.offsetMin = Vector2.zero;
        vpRT.offsetMax = Vector2.zero;
        scrollRect.viewport = vpRT;

        var content = new GameObject("Content");
        content.transform.SetParent(viewport.transform, false);
        var cRT = content.AddComponent<RectTransform>();
        cRT.anchorMin = new Vector2(0f, 1f);
        cRT.anchorMax = new Vector2(1f, 1f);
        cRT.pivot = new Vector2(0.5f, 1f);
        cRT.offsetMin = Vector2.zero;
        cRT.offsetMax = Vector2.zero;
        cRT.sizeDelta = new Vector2(0, 740);
        scrollRect.content = cRT;

        var textGO = new GameObject("ChatText");
        textGO.transform.SetParent(content.transform, false);
        var trt = textGO.AddComponent<RectTransform>();
        trt.anchorMin = new Vector2(0f, 1f);
        trt.anchorMax = new Vector2(1f, 1f);
        trt.pivot = new Vector2(0.5f, 1f);
        trt.anchoredPosition = new Vector2(0, -18);
        trt.sizeDelta = new Vector2(0, 380);
        var textTMP = textGO.AddComponent<TextMeshProUGUI>();
        textTMP.text = "<size=13><color=#02D7F2>[Aoi]</color></size>\n就绪。按住 Grip 说话。";
        textTMP.fontSize = 16.5f;
        textTMP.lineSpacing = 7f;
        textTMP.alignment = TextAlignmentOptions.TopLeft;
        textTMP.color = TextMain;
        textTMP.font = AoiOrchestrator.ResolveUIFont();
        textTMP.enableWordWrapping = true;
        textTMP.overflowMode = TextOverflowModes.Overflow;
        textTMP.richText = true;
        textTMP.raycastTarget = false;

        return page;
    }

    // ---------- scroll buttons + track ----------
    GameObject CreateScrollUI(Transform parent,
        out RectTransform trackRT, out RectTransform thumbRT,
        out Button btnUp, out Button btnDown)
    {
        // track: right 8 from board right, 3 wide, top 104 bottom 182, cyan 0.12
        var trackGO = new GameObject("ScrollTrack");
        trackGO.transform.SetParent(parent, false);
        trackRT = trackGO.AddComponent<RectTransform>();
        trackRT.anchorMin = new Vector2(1f, 0f);
        trackRT.anchorMax = new Vector2(1f, 1f);
        trackRT.offsetMin = new Vector2(-11, 104);
        trackRT.offsetMax = new Vector2(-8, -182);
        var trackImg = trackGO.AddComponent<Image>();
        trackImg.color = new Color(0.008f, 0.843f, 0.949f, 0.12f);
        trackImg.raycastTarget = false;

        var thumbGO = new GameObject("ScrollThumb");
        thumbGO.transform.SetParent(trackGO.transform, false);
        thumbRT = thumbGO.AddComponent<RectTransform>();
        thumbRT.anchorMin = new Vector2(0.5f, 1f);
        thumbRT.anchorMax = new Vector2(0.5f, 1f);
        thumbRT.pivot = new Vector2(0.5f, 1f);
        thumbRT.anchoredPosition = Vector2.zero;
        thumbRT.sizeDelta = new Vector2(11, Board - 104 - 182);
        var thumbImg = thumbGO.AddComponent<Image>();
        thumbImg.sprite = MakeBarGlowSprite(11, 3, Cyan);
        thumbImg.type = Image.Type.Simple;
        thumbImg.preserveAspect = false;
        thumbImg.raycastTarget = false;

        // buttons: 42 square, cut 8, right 14, vertical center, gap 12
        // up center (649, 315) down center (649, 369)
        btnUp = CreateScrollBtn(parent, "ChatScrollUp", "▲",
            new Vector2(649f - Board / 2f, Board / 2f - 315f));
        btnDown = CreateScrollBtn(parent, "ChatScrollDown", "▼",
            new Vector2(649f - Board / 2f, Board / 2f - 369f));

        return trackGO;
    }

    Button CreateScrollBtn(Transform parent, string name, string glyph, Vector2 pos)
    {
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = new Vector2(0.5f, 0.5f);
        rt.anchorMax = new Vector2(0.5f, 0.5f);
        rt.anchoredPosition = pos;
        rt.sizeDelta = new Vector2(42, 42);
        var img = go.AddComponent<Image>();
        img.sprite = MakeCutPanel(42, 42, 8, 1, CyanDim, new Color(0.008f, 0.843f, 0.949f, 0.08f));
        img.type = Image.Type.Simple;
        img.preserveAspect = false;
        var btn = go.AddComponent<Button>();
        btn.targetGraphic = img;
        btn.transition = Selectable.Transition.None;
        var txt = CreateText(go.transform, "Label", glyph, 16, TextAlignmentOptions.Center, Cyan, false);
        txt.font = AoiOrchestrator.ResolveMonoFont();
        var tRT = txt.GetComponent<RectTransform>();
        tRT.anchorMin = Vector2.zero; tRT.anchorMax = Vector2.one;
        tRT.offsetMin = Vector2.zero; tRT.offsetMax = Vector2.zero;
        return btn;
    }

    // ---------- subtitle bar (bottom 112, width 82%, amber border, glow halo) ----------
    GameObject CreateSubtitleBar(Transform parent,
        out TextMeshProUGUI subtitleText, out TextMeshProUGUI subtitleTagText)
    {
        // glow halo band behind the bar
        var glow = new GameObject("SubtitleGlow");
        glow.transform.SetParent(parent, false);
        var glowRT = glow.AddComponent<RectTransform>();
        glowRT.anchorMin = new Vector2(0.5f, 0f);
        glowRT.anchorMax = new Vector2(0.5f, 0f);
        glowRT.pivot = new Vector2(0.5f, 0f);
        glowRT.anchoredPosition = new Vector2(0, 108);
        glowRT.sizeDelta = new Vector2(0.84f * Board, 78);
        var glowImg = glow.AddComponent<Image>();
        glowImg.color = new Color(1f, 0.843f, 0.369f, 0.08f);
        glowImg.raycastTarget = false;

        var go = new GameObject("SubtitleBar");
        go.transform.SetParent(glow.transform, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = Vector2.zero;
        rt.anchorMax = Vector2.one;
        rt.offsetMin = new Vector2(4, 4);
        rt.offsetMax = new Vector2(-4, -4);
        // fill (inset 1) + 1px amber line border (interior of the border itself)
        var fill = go.AddComponent<Image>();
        fill.color = SubFill;
        fill.raycastTarget = false;
        CreateBorderLines(go.transform, new Color(1f, 0.843f, 0.369f, 0.5f));

        // tag: font 10.5, tracking 22%, amber 0.6, at (24, -11) from top-left
        subtitleTagText = CreateText(go.transform, "Tag", "实时字幕翻译 → 中文",
            10.5f, TextAlignmentOptions.MidlineLeft, new Color(1f, 0.843f, 0.369f, 0.6f), false);
        subtitleTagText.characterSpacing = 22f;
        subtitleTagText.font = AoiOrchestrator.ResolveMonoFont();
        var tagRT = subtitleTagText.GetComponent<RectTransform>();
        tagRT.anchorMin = new Vector2(0, 1); tagRT.anchorMax = new Vector2(0, 1);
        tagRT.pivot = new Vector2(0, 1);
        tagRT.anchoredPosition = new Vector2(24, -11);
        tagRT.sizeDelta = new Vector2(500, 16);

        var closeText = CreateText(go.transform, "CloseHint", "说「停止翻译」关闭",
            10.5f, TextAlignmentOptions.MidlineRight, new Color(1f, 0.843f, 0.369f, 0.45f), false);
        closeText.characterSpacing = 22f;
        closeText.font = AoiOrchestrator.ResolveMonoFont();
        var closeRT = closeText.GetComponent<RectTransform>();
        closeRT.anchorMin = new Vector2(1, 1); closeRT.anchorMax = new Vector2(1, 1);
        closeRT.pivot = new Vector2(1, 1);
        closeRT.anchoredPosition = new Vector2(-24, -11);
        closeRT.sizeDelta = new Vector2(400, 16);

        var txt = new GameObject("SubtitleText");
        txt.transform.SetParent(go.transform, false);
        var textRT = txt.AddComponent<RectTransform>();
        textRT.anchorMin = Vector2.zero;
        textRT.anchorMax = Vector2.one;
        textRT.offsetMin = new Vector2(24, 12);
        textRT.offsetMax = new Vector2(-24, -34);

        subtitleText = txt.AddComponent<TextMeshProUGUI>();
        subtitleText.text = "";
        subtitleText.fontSize = 17;
        subtitleText.lineSpacing = 6f;
        subtitleText.alignment = TextAlignmentOptions.BottomLeft;
        subtitleText.color = Amber;
        subtitleText.font = AoiOrchestrator.ResolveUIFont();
        subtitleText.enableWordWrapping = true;
        subtitleText.overflowMode = TextOverflowModes.Truncate;
        subtitleText.raycastTarget = false;

        glow.SetActive(false);
        return glow;
    }

    // ---------- hint bar (bottom 28, font 13, tracking 12%, dim, centered) ----------
    GameObject CreateHintBar(Transform parent, out TextMeshProUGUI hintText)
    {
        var go = new GameObject("HintBar");
        go.transform.SetParent(parent, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = new Vector2(0.5f, 0f);
        rt.anchorMax = new Vector2(0.5f, 0f);
        rt.pivot = new Vector2(0.5f, 0f);
        rt.anchoredPosition = new Vector2(0, 28);
        rt.sizeDelta = new Vector2(Board, 46);

        hintText = go.AddComponent<TextMeshProUGUI>();
        hintText.text = "";
        hintText.fontSize = 13;
        hintText.characterSpacing = 12f;
        hintText.font = AoiOrchestrator.ResolveMonoFont();
        hintText.alignment = TextAlignmentOptions.Center;
        hintText.color = TextDim;
        hintText.font = AoiOrchestrator.ResolveUIFont();
        hintText.enableWordWrapping = true;
        hintText.overflowMode = TextOverflowModes.Overflow;
        hintText.richText = true;
        hintText.raycastTarget = false;

        return go;
    }

    // ---------- screenshot toast (top 76, right 30, 200.77x64, cut 8) ----------
    GameObject CreateShotToast(Transform parent,
        out TextMeshProUGUI title, out TextMeshProUGUI sub)
    {
        var glow = new GameObject("ShotToastGlow");
        glow.transform.SetParent(parent, false);
        var glowRT = glow.AddComponent<RectTransform>();
        glowRT.anchorMin = new Vector2(1, 1);
        glowRT.anchorMax = new Vector2(1, 1);
        glowRT.pivot = new Vector2(1, 1);
        glowRT.anchoredPosition = new Vector2(-26, -72);
        glowRT.sizeDelta = new Vector2(209, 72);
        var glowImg = glow.AddComponent<Image>();
        glowImg.color = new Color(0.008f, 0.843f, 0.949f, 0.12f);
        glowImg.raycastTarget = false;

        var go = new GameObject("ShotToast");
        go.transform.SetParent(glow.transform, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = Vector2.zero;
        rt.anchorMax = Vector2.one;
        rt.offsetMin = new Vector2(4, 4);
        rt.offsetMax = new Vector2(-4, -4);
        var border = go.AddComponent<Image>();
        border.sprite = MakeCutPanel(200.77f, 64, 8, 1, CyanDim, ToastFill);
        border.type = Image.Type.Simple;
        border.preserveAspect = false;
        border.raycastTarget = false;

        // thumb: x=17, 44 square, border cyan-dim, fill cyan 0.18, icon ▣ 18
        var thumb = new GameObject("Thumb");
        thumb.transform.SetParent(go.transform, false);
        var thumbRT = thumb.AddComponent<RectTransform>();
        thumbRT.anchorMin = new Vector2(0, 0.5f); thumbRT.anchorMax = new Vector2(0, 0.5f);
        thumbRT.pivot = new Vector2(0, 0.5f);
        thumbRT.anchoredPosition = new Vector2(17, 0);
        thumbRT.sizeDelta = new Vector2(44, 44);
        var thumbBorder = thumb.AddComponent<Image>();
        thumbBorder.color = CyanDim;
        thumbBorder.raycastTarget = false;
        var thumbInner = new GameObject("Inner");
        thumbInner.transform.SetParent(thumb.transform, false);
        var tiRT = thumbInner.AddComponent<RectTransform>();
        tiRT.anchorMin = Vector2.zero; tiRT.anchorMax = Vector2.one;
        tiRT.offsetMin = new Vector2(1, 1); tiRT.offsetMax = new Vector2(-1, -1);
        var tiImg = thumbInner.AddComponent<Image>();
        tiImg.color = new Color(0.008f, 0.843f, 0.949f, 0.18f);
        tiImg.raycastTarget = false;
        // small filled-square icon (no font glyph dependency)
        var iconSq = new GameObject("IconSq");
        iconSq.transform.SetParent(thumbInner.transform, false);
        var sqRT = iconSq.AddComponent<RectTransform>();
        sqRT.anchorMin = new Vector2(0.5f, 0.5f); sqRT.anchorMax = new Vector2(0.5f, 0.5f);
        sqRT.anchoredPosition = Vector2.zero;
        sqRT.sizeDelta = new Vector2(18, 18);
        var sqImg = iconSq.AddComponent<Image>();
        sqImg.color = Cyan;
        sqImg.raycastTarget = false;

        // title: x=67, font 12.5 tracking 10%, cyan
        title = CreateText(go.transform, "Title", "已截图", 12.5f, TextAlignmentOptions.MidlineLeft, Cyan, false);
        title.characterSpacing = 10f;
        title.font = AoiOrchestrator.ResolveMonoFont();
        var titleRT = title.GetComponent<RectTransform>();
        titleRT.anchorMin = new Vector2(0, 0.5f); titleRT.anchorMax = new Vector2(1, 0.5f);
        titleRT.offsetMin = new Vector2(71, 2);
        titleRT.offsetMax = new Vector2(-6, 20);

        // sub: font 10.5, dim
        sub = CreateText(go.transform, "Sub", "", 10.5f, TextAlignmentOptions.MidlineLeft, TextDim, false);
        sub.font = AoiOrchestrator.ResolveMonoFont();
        var subRT = sub.GetComponent<RectTransform>();
        subRT.anchorMin = new Vector2(0, 0.5f); subRT.anchorMax = new Vector2(1, 0.5f);
        subRT.offsetMin = new Vector2(71, -20);
        subRT.offsetMax = new Vector2(-6, -4);

        glow.SetActive(false);
        return glow;
    }

    // ---------- generic builders ----------
    GameObject CreateRect(Transform parent, string name, Color color,
        Vector2 aMin, Vector2 aMax, Vector2 anchoredPos, Vector2 sizeDelta)
    {
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = aMin; rt.anchorMax = aMax;
        rt.anchoredPosition = anchoredPos;
        rt.sizeDelta = sizeDelta;
        var img = go.AddComponent<Image>();
        img.color = color;
        img.raycastTarget = false;
        return go;
    }

    TextMeshProUGUI CreateText(Transform parent, string name, string text,
        float fontSize, TextAlignmentOptions alignment, Color color, bool richText)
    {
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        var rt = go.AddComponent<RectTransform>();
        rt.anchorMin = new Vector2(0.5f, 0.5f); rt.anchorMax = new Vector2(0.5f, 0.5f);
        rt.anchoredPosition = Vector2.zero;
        rt.sizeDelta = new Vector2(200, 40);

        var txt = go.AddComponent<TextMeshProUGUI>();
        txt.text = text;
        txt.fontSize = fontSize;
        txt.alignment = alignment;
        txt.color = color;
        txt.font = AoiOrchestrator.ResolveUIFont();
        txt.enableWordWrapping = true;
        txt.overflowMode = TextOverflowModes.Overflow;
        txt.richText = richText;
        txt.raycastTarget = false;
        return txt;
    }
}
