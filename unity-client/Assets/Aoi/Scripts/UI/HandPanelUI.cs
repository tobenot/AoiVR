using UnityEngine;
using UnityEngine.UI;
using TMPro;

public class HandPanelUI : MonoBehaviour
{
    [Header("Status Bar")]
    public TextMeshProUGUI logoText;
    public TextMeshProUGUI logoVerText;
    public TextMeshProUGUI statusText;
    public Image statusDot;
    public GameObject interpChip;
    public TextMeshProUGUI interpChipText;
    public GameObject envChip;
    public TextMeshProUGUI envChipText;

    [Header("Processing Pipeline (ephemeral)")]
    public GameObject pipelineContainer;
    public TextMeshProUGUI procbarText;
    public CanvasGroup procbarCanvasGroup;
    public Image[] pstepDots = new Image[3];
    public TextMeshProUGUI[] pstepLabels = new TextMeshProUGUI[3];
    public RectTransform[] pstepRects = new RectTransform[3];   // for dynamic x positions
    public TextMeshProUGUI[] pstepConns = new TextMeshProUGUI[2]; // "→" between steps

    [Header("Chat")]
    public TextMeshProUGUI chatText;
    public ScrollRect chatScroll;
    public Button chatScrollUp;
    public Button chatScrollDown;
    public RectTransform scrollTrack;
    public RectTransform scrollThumb;

    // Simple running history of assistant replies (only AI text, no user).
    private System.Text.StringBuilder chatHistory = new System.Text.StringBuilder();

    [Header("Subtitle Bar")]
    public GameObject subtitleBar;
    public TextMeshProUGUI subtitleText;
    public TextMeshProUGUI subtitleTagText;

    private string[] subtitleLines = new string[0];
    private int maxSubtitleLines = 3;

    [Header("Hint Bar")]
    public TextMeshProUGUI hintText;

    [Header("Screenshot Toast")]
    public GameObject shotToast;
    public TextMeshProUGUI shotToastTitle;
    public TextMeshProUGUI shotToastSub;
    public float shotToastSeconds = 3f;
    private float shotToastHideTime = -1f;

    [Header("Colors")]
    public Color statusReadyColor = new Color(0.008f, 0.843f, 0.949f);   // #02D7F2
    public Color statusBusyColor = new Color(0.953f, 0.902f, 0f);        // #F3E600
    public Color statusErrorColor = new Color(1f, 0.09f, 0.267f);        // #FF1744

    public System.Action<string> onLog;

    void Start()
    {
        if (chatScrollUp != null) chatScrollUp.onClick.AddListener(ScrollUp);
        if (chatScrollDown != null) chatScrollDown.onClick.AddListener(ScrollDown);
        if (shotToast != null) shotToast.SetActive(false);
        if (interpChip != null) interpChip.SetActive(false);
        if (envChip != null) envChip.SetActive(false);
        var dots = new System.Collections.Generic.List<Image>();
        foreach (var chip in new[] { interpChip, envChip })
        {
            if (chip == null) continue;
            var d = chip.transform.Find("Dot")?.GetComponent<Image>();
            if (d != null) dots.Add(d);
        }
        chipDots = dots.ToArray();
    }

    void Update()
    {
        if (shotToastHideTime > 0f && Time.unscaledTime >= shotToastHideTime)
        {
            shotToastHideTime = -1f;
            if (shotToast != null) shotToast.SetActive(false);
        }
        UpdateScrollThumb();
        UpdateDotPulse();
    }

    private Image[] chipDots;
    private int currentColorState = 0;

    // Status dot pulses while busy (mockup: opacity .45 <-> 1 at 1s period);
    // chip dots pulse constantly while their chip is visible.
    void UpdateDotPulse()
    {
        float pulse = 0.725f + 0.275f * Mathf.Sin(Time.unscaledTime * Mathf.PI * 2f);
        if (statusDot != null)
        {
            var c = statusDot.color;
            c.a = currentColorState == 1 ? pulse : 1f;
            statusDot.color = c;
        }
        if (chipDots != null)
        {
            foreach (var d in chipDots)
            {
                if (d == null) continue;
                var c = d.color;
                c.a = pulse;
                d.color = c;
            }
        }
    }

    // Scroll position indicator (track + thumb on the chat area's right edge).
    void UpdateScrollThumb()
    {
        if (scrollThumb == null || scrollTrack == null || chatScroll == null) return;
        var content = chatScroll.content;
        var viewport = chatScroll.viewport;
        if (content == null || viewport == null) return;
        float vh = viewport.rect.height;
        float ch = content.rect.height;
        float ratio = ch <= vh ? 1f : Mathf.Clamp01(vh / ch);
        float trackH = scrollTrack.rect.height;
        if (trackH <= 0f) return;
        float thumbH = Mathf.Max(24f, trackH * ratio);
        var size = scrollThumb.sizeDelta;
        size.y = thumbH;
        scrollThumb.sizeDelta = size;
        // verticalNormalizedPosition: 1 = top (oldest), 0 = bottom (newest).
        float norm = chatScroll.verticalNormalizedPosition;
        float y = -(trackH - thumbH) * (1f - norm);
        var pos = scrollThumb.anchoredPosition;
        pos.y = y;
        scrollThumb.anchoredPosition = pos;
    }

    // ---- Status (mic/conversation state: ready / recording / sending / error) ----
    // colorState: 0 = ready(cyan), 1 = busy(yellow), 2 = error(magenta)
    public void SetStatus(string text, int colorState = 0)
    {
        currentColorState = colorState;
        if (statusText != null)
        {
            statusText.text = text;
            statusText.color = ColorForState(colorState);
        }
        if (statusDot != null) statusDot.color = ColorForState(colorState);
        RelayoutStatusBar();
    }

    Color ColorForState(int s)
    {
        switch (s)
        {
            case 1: return statusBusyColor;
            case 2: return statusErrorColor;
            default: return statusReadyColor;
        }
    }

    // ---- Background state chips (never overwritten by mic status) ----
    public void SetInterpActive(bool active, string targetLang = null)
    {
        if (interpChip != null) interpChip.SetActive(active);
        if (active && interpChipText != null && !string.IsNullOrEmpty(targetLang))
            interpChipText.text = $"实时字幕 →{targetLang}";
        if (subtitleTagText != null && !string.IsNullOrEmpty(targetLang))
            subtitleTagText.text = $"实时字幕翻译 → {targetLang}";
        RelayoutStatusBar();
    }

    public void SetEnvActive(bool active)
    {
        if (envChip != null) envChip.SetActive(active);
        RelayoutStatusBar();
    }

    // Flex-like status bar layout (from the mockup): logo at x=31, then 12px gap,
    // state container (dot 9 + gap 9 + text), then 12px gap, then visible chips
    // left to right, each chip width = label preferred width + 38.
    void RelayoutStatusBar()
    {
        if (statusText == null) return;
        statusText.ForceMeshUpdate();
        float logoW = 126f;
        if (logoText != null)
        {
            logoText.ForceMeshUpdate();
            logoW = logoText.preferredWidth;
        }
        if (logoVerText != null)
        {
            logoVerText.ForceMeshUpdate();
            logoW += 8f + logoVerText.preferredWidth;
        }
        float stateX = 31f + logoW + 12f;
        if (statusDot != null)
        {
            var dotRT = statusDot.GetComponent<RectTransform>();
            var dp = dotRT.anchoredPosition;
            dp.x = stateX + 4.5f;
            dotRT.anchoredPosition = dp;
        }
        var stRT = statusText.GetComponent<RectTransform>();
        var sp = stRT.anchoredPosition;
        sp.x = stateX + 9f + 9f;
        stRT.anchoredPosition = sp;

        float x = sp.x + statusText.preferredWidth + 40f;
        // Processing pipeline sits right after the status text (mockup layout:
        // ●就绪 ●理解 → ●检索 → ●生成), before any background chips.
        if (pipelineContainer != null && pipelineContainer.activeSelf)
        {
            var pipeRT = pipelineContainer.GetComponent<RectTransform>();
            var pp = pipeRT.anchoredPosition;
            pp.x = 280f;  // fixed: right after "就绪" text, mockup-like spacing
            pipeRT.anchoredPosition = pp;
            x = 280f + 260f + 40f;
        }
        if (interpChip != null && interpChip.activeSelf)
        {
            x = PlaceChip(interpChip, interpChipText, x);
        }
        if (envChip != null && envChip.activeSelf)
        {
            x = PlaceChip(envChip, envChipText, x);
        }
    }

    float PlaceChip(GameObject chip, TextMeshProUGUI label, float x)
    {
        float w = 120f;
        if (label != null)
        {
            label.ForceMeshUpdate();
            w = label.preferredWidth + 38f;
        }
        var rt = chip.GetComponent<RectTransform>();
        var pos = rt.anchoredPosition;
        pos.x = x;
        rt.anchoredPosition = pos;
        var size = rt.sizeDelta;
        size.x = w;
        rt.sizeDelta = size;
        return x + w + 12f;
    }

    // ---- Hint bar (contextual gesture help) ----
    public void SetHint(string richText)
    {
        if (hintText != null) hintText.text = richText ?? "";
    }

    // ---- Processing pipeline (understand -> retrieve -> generate) ----
    // stage: "understand" | "retrieve" | "generate" | "done"
    // Pipeline is ephemeral: shown while the agent is working, then fades out.
    private float procFadeStart = -1f;
    private const float procFadeDuration = 0.5f;
    public void SetProcessingStage(string stage, string thought = null)
    {
        if (procbarText == null) return;
        if (stage == "done")
        {
            procFadeStart = Time.unscaledTime;
            return;
        }
        ShowProcessing(true);

        // thought summary replaces in place (mockup: "▸ 他在问这家店还开不开…")
        procbarText.text = "▸ " + (string.IsNullOrEmpty(thought)
            ? (stage switch { "understand" => "正在理解…", "retrieve" => "正在检索…", _ => "正在生成…" })
            : thought);
        // Width: min 300 (default length), max fills the row (684 - 34*2 margins).
        // If the text doesn't fit, Ellipsis overflow truncates the tail.
        procbarText.ForceMeshUpdate();
        var procbar = procbarText.transform.parent;
        var rt = procbar.GetComponent<RectTransform>();
        var img = procbar.GetComponent<UnityEngine.UI.Image>();
        var size = rt.sizeDelta;
        float w = Mathf.Clamp(procbarText.preferredWidth + 24f, 300f, 616f);
        if (Mathf.Abs(size.x - w) > 0.5f)
        {
            size.x = w;
            rt.sizeDelta = size;
            if (img != null)
                img.sprite = AoiBootstrap.MakeCutPanel2Corners(w, 34, 8, 1,
                    new Color(0.953f, 0.902f, 0f, 0.4f),
                    new Color(0.016f, 0.024f, 0.055f, 0.9f));
        }
    }

    public void ShowProcessing(bool show)
    {
        if (pipelineContainer != null) pipelineContainer.SetActive(show);
        if (procbarCanvasGroup != null)
        {
            procbarCanvasGroup.alpha = 1f;
            procbarCanvasGroup.gameObject.SetActive(show);
        }
    }

    // Immediately hide the processing bar (called when a reply/error arrives,
    // so the UI never stays on "正在理解…" even if a processing_stage "done"
    // was lost on an error path).
    public void ResetProcessing()
    {
        procFadeStart = -1f;
        ShowProcessing(false);
    }

    public void UpdateProcessing()
    {
        if (procFadeStart >= 0f && procbarCanvasGroup != null)
        {
            float t = Time.unscaledTime - procFadeStart;
            if (t >= procFadeDuration)
            {
                procbarCanvasGroup.alpha = 0f;
                procbarCanvasGroup.gameObject.SetActive(false);
                procFadeStart = -1f;
                ShowProcessing(false);
            }
            else
            {
                procbarCanvasGroup.alpha = 1f - t / procFadeDuration;
            }
        }
    }

    // ---- Screenshot toast ----
    public void ShowShotToast(string title, string sub = null)
    {
        if (shotToast == null) return;
        if (shotToastTitle != null) shotToastTitle.text = title ?? "已截图";
        if (shotToastSub != null)
        {
            shotToastSub.text = sub ?? "";
            shotToastSub.gameObject.SetActive(!string.IsNullOrEmpty(sub));
        }
        shotToast.SetActive(true);
        shotToastHideTime = Time.unscaledTime + shotToastSeconds;
    }

    // ---- Chat ----
    // Append an assistant reply to the running history.
    // partial=true  -> replace the current in-progress reply (streaming update)
    // partial=false -> commit the reply as a new history entry
    public void AppendChatText(string text, bool isFinal)
    {
        if (chatText == null) return;
        if (string.IsNullOrEmpty(text)) text = "";
        text = MarkdownToRichText(text);

        if (isFinal)
        {
            // Don't append an empty [Aoi] label for a blank final reply — it
            // would leave a dangling tag with nothing under it.
            if (string.IsNullOrEmpty(text)) return;
            if (chatHistory.Length > 0) chatHistory.Append("\n\n");
            chatHistory.Append("<size=13><color=#02D7F2>[Aoi]</color></size>\n");
            chatHistory.Append(text);
            RefreshChatDisplay(scrollToBottom: true);
        }
        else
        {
            var preview = chatHistory.ToString();
            if (preview.Length > 0) preview += "\n\n";
            preview += "<size=13><color=#02D7F2>[Aoi]</color></size>\n" + text;
            SetDisplayText(preview, scrollToBottom: true);
        }
    }

    // Error message: red [错误] label + red text, so failures stand out from
    // normal replies and the user knows the request did not go through.
    public void AppendErrorText(string text)
    {
        if (chatText == null) return;
        if (string.IsNullOrEmpty(text)) return;
        if (chatHistory.Length > 0) chatHistory.Append("\n\n");
        chatHistory.Append("<size=13><color=#FF5A5A>[错误]</color></size>\n");
        chatHistory.Append("<color=#FFB3B3>" + MarkdownToRichText(text) + "</color>");
        RefreshChatDisplay(scrollToBottom: true);
    }

    // User message: committed as its own history entry (used by demo mode and
    // future user-transcript features).
    public void AppendUserText(string text)
    {
        if (chatText == null) return;
        if (string.IsNullOrEmpty(text)) return;
        if (chatHistory.Length > 0) chatHistory.Append("\n\n");
        chatHistory.Append("<size=13><color=#B8B8B8>[User]</color></size>\n");
        chatHistory.Append("<color=#E8E8E8>" + MarkdownToRichText(text) + "</color>");
        RefreshChatDisplay(scrollToBottom: true);
    }

    // Backwards-compatible: replaces the whole panel with a single text.
    public void SetChatText(string text)
    {
        if (chatText == null) return;
        chatHistory.Clear();
        chatHistory.Append(MarkdownToRichText(text ?? ""));
        RefreshChatDisplay(scrollToBottom: true);
    }

    public void ClearChatText()
    {
        chatHistory.Clear();
        SetDisplayText("", scrollToBottom: true);
    }

    void RefreshChatDisplay(bool scrollToBottom)
    {
        SetDisplayText(chatHistory.ToString(), scrollToBottom);
    }

    void SetDisplayText(string richText, bool scrollToBottom)
    {
        chatText.text = richText ?? "";
        chatText.ForceMeshUpdate();
        // ChatText is x-stretched to the content; only the height is manual.
        var textH = Mathf.Max(100f, chatText.preferredHeight);
        var size = chatText.rectTransform.sizeDelta;
        size.y = textH;
        chatText.rectTransform.sizeDelta = size;
        var contentRT = chatText.rectTransform.parent as RectTransform;
        if (contentRT != null)
        {
            var csize = contentRT.sizeDelta;
            csize.y = textH + 40;
            contentRT.sizeDelta = csize;
        }
        if (scrollToBottom) ScrollToBottom();
    }

    void ScrollToBottom()
    {
        if (chatScroll == null) return;
        Canvas.ForceUpdateCanvases();
        chatScroll.verticalNormalizedPosition = 0f;
    }

    // ▲▼ buttons scroll 0.4 of the viewport per click (matching the mockup's
    // scrollTop ± clientHeight * 0.4 behavior).
    float StepFraction()
    {
        if (chatScroll == null || chatScroll.viewport == null || chatScroll.content == null) return 0f;
        float v = chatScroll.viewport.rect.height;
        float range = chatScroll.content.rect.height - v;
        if (range <= 0f) return 0f;
        return 0.4f * v / range;
    }

    public void ScrollUp()
    {
        if (chatScroll == null) return;
        Canvas.ForceUpdateCanvases();
        // verticalNormalizedPosition: 1 = top (older), 0 = bottom (newest).
        chatScroll.verticalNormalizedPosition = Mathf.Clamp01(chatScroll.verticalNormalizedPosition + StepFraction());
    }

    public void ScrollDown()
    {
        if (chatScroll == null) return;
        Canvas.ForceUpdateCanvases();
        chatScroll.verticalNormalizedPosition = Mathf.Clamp01(chatScroll.verticalNormalizedPosition - StepFraction());
    }

    // Minimal markdown -> TextMeshPro rich text converter.
    // Supports: # headings, **bold**, *italic*, `inline code`, ```fenced code
    // blocks```, - / * / numbered lists, > quotes, and [text](url) links.
    public static string MarkdownToRichText(string md)
    {
        if (string.IsNullOrEmpty(md)) return md;

        var lines = md.Replace("\r\n", "\n").Split('\n');
        var sb = new System.Text.StringBuilder();
        bool inCodeBlock = false;
        for (int i = 0; i < lines.Length; i++)
        {
            var line = lines[i];
            var trimmed = line.Trim();

            if (trimmed.StartsWith("```"))
            {
                if (!inCodeBlock)
                {
                    inCodeBlock = true;
                    sb.Append("<mspace=1.2em><color=#9AD1FF>").Append(line).Append('\n');
                }
                else
                {
                    inCodeBlock = false;
                    sb.Append("</color></mspace>\n");
                }
                continue;
            }
            if (inCodeBlock)
            {
                sb.Append(EscapeRichText(line)).Append('\n');
                continue;
            }

            if (trimmed.StartsWith("###"))
                sb.Append("<size=1.25em><b><color=#7FD1FF>").Append(Inline(trimmed.Substring(3).Trim())).Append("</color></b></size>\n");
            else if (trimmed.StartsWith("##"))
                sb.Append("<size=1.15em><b><color=#7FD1FF>").Append(Inline(trimmed.Substring(2).Trim())).Append("</color></b></size>\n");
            else if (trimmed.StartsWith("#"))
                sb.Append("<size=1.3em><b><color=#7FD1FF>").Append(Inline(trimmed.Substring(1).Trim())).Append("</color></b></size>\n");
            else if (trimmed.StartsWith("&gt;"))
                sb.Append("<color=#8FA3BF><i>").Append(Inline(trimmed.Substring(4).Trim())).Append("</i></color>\n");
            else if (trimmed.StartsWith("- ") || trimmed.StartsWith("* "))
                sb.Append("  <color=#00C8FF>•</color> ").Append(Inline(trimmed.Substring(2).Trim())).Append('\n');
            else if (System.Text.RegularExpressions.Regex.IsMatch(trimmed, @"^\d+[\.\)]\s"))
            {
                int dot = trimmed.IndexOfAny(new[] { '.', ')' });
                sb.Append("  <color=#00C8FF>").Append(trimmed.Substring(0, dot + 1)).Append("</color> ").Append(Inline(trimmed.Substring(dot + 1).Trim())).Append('\n');
            }
            else
            {
                sb.Append(Inline(line)).Append('\n');
            }
        }
        var result = sb.ToString().TrimEnd('\n');
        return result;
    }

    // Handle inline markdown: **bold**, *italic*, `code`, [text](url).
    static string Inline(string s)
    {
        if (string.IsNullOrEmpty(s)) return s;
        s = EscapeRichText(s);

        // Bold **text** (handle before italic so ** wins over *)
        s = System.Text.RegularExpressions.Regex.Replace(s, @"\*\*(.+?)\*\*", "<b>$1</b>");
        // Italic *text*
        s = System.Text.RegularExpressions.Regex.Replace(s, @"\*(.+?)\*", "<i>$1</i>");
        // Inline code `text`
        s = System.Text.RegularExpressions.Regex.Replace(s, @"`(.+?)`", "<mspace=1.1em><color=#9AD1FF>$1</color></mspace>");
        // Links [text](url) -> keep text
        s = System.Text.RegularExpressions.Regex.Replace(s, @"\[([^\]]+)\]\([^)]+\)", "$1");
        return s;
    }

    // Escape characters that TMP interprets as rich-text tags.
    static string EscapeRichText(string s)
    {
        return s.Replace("<", "&lt;").Replace(">", "&gt;");
    }

    // ---- Subtitle bar (live caption translation, background state) ----
    public void SetSubtitleActive(bool active)
    {
        if (subtitleBar != null) subtitleBar.SetActive(active);
        if (!active)
        {
            subtitleLines = new string[0];
            if (subtitleText != null) subtitleText.text = "";
        }
        ResizeSubtitleBar();
    }

    public void AddSubtitle(string text)
    {
        if (subtitleText == null || string.IsNullOrEmpty(text)) return;

        // Escape before storing so TMP <tags> from the model can't inject/break
        // subtitle styling (the chat path already escapes; subtitles did not).
        var safe = EscapeRichText(text);
        var lines = new System.Collections.Generic.List<string>(subtitleLines);
        lines.Add(safe);
        if (lines.Count > maxSubtitleLines)
            lines.RemoveAt(0);
        subtitleLines = lines.ToArray();
        subtitleText.text = string.Join("\n", subtitleLines);
        ResizeSubtitleBar();
    }

    // Bar height grows with line count (tag row 27 + 12 bottom pad + n lines x 26.35).
    void ResizeSubtitleBar()
    {
        if (subtitleBar == null) return;
        var rt = subtitleBar.GetComponent<RectTransform>();
        if (rt == null) return;
        int n = Mathf.Max(1, subtitleLines.Length);
        float contentH = 27f + 12f + n * 26.35f;
        // SubtitleBar is the glow wrapper's child (inset 4); wrapper height = content + 8.
        var size = rt.sizeDelta;
        size.y = contentH + 8f;
        rt.sizeDelta = size;
    }
}
