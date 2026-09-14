using System.IO;
using System.Linq;
using System.Collections.Generic;
using TMPro;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;

public static class BuildScript
{
    [MenuItem("Build/Import TMP Official")]
    public static void ImportTMPOfficial()
    {
        string pkgPath = "Packages/com.unity.ugui/Package Resources/TMP Essential Resources.unitypackage";
        if (!File.Exists(pkgPath))
        {
            Debug.LogError("TMP package not found at " + pkgPath);
            return;
        }
        AssetDatabase.importPackageCompleted += OnTmpImportDone;
        AssetDatabase.importPackageFailed += OnTmpImportFail;
        AssetDatabase.importPackageCancelled += OnTmpImportCancel;
        AssetDatabase.ImportPackage(pkgPath, false);
        Debug.Log("ImportPackage started (async): " + pkgPath);
    }

    static void OnTmpImportDone(string pkgName)
    {
        AssetDatabase.importPackageCompleted -= OnTmpImportDone;
        AssetDatabase.importPackageFailed -= OnTmpImportFail;
        AssetDatabase.importPackageCancelled -= OnTmpImportCancel;
        Debug.Log("TMP Essentials import COMPLETED: " + pkgName);
        EditorApplication.Exit(0);
    }

    static void OnTmpImportFail(string pkgName, string err)
    {
        Debug.LogError("TMP Essentials import FAILED: " + pkgName + " err=" + err);
        EditorApplication.Exit(1);
    }

    static void OnTmpImportCancel(string pkgName)
    {
        Debug.LogError("TMP Essentials import CANCELLED: " + pkgName);
        EditorApplication.Exit(1);
    }

    [MenuItem("Build/Diagnose Font")]
    public static void DiagnoseFont()
    {
        var font = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>("Assets/TextMesh Pro/Resources/Fonts & Materials/LiberationSans SDF.asset");
        if (font == null) { Debug.LogError("FONT: not found"); return; }
        Debug.Log($"FONT: name={font.name} mode={font.atlasPopulationMode} atlas={font.atlasTexture.width}x{font.atlasTexture.height} glyphs={font.glyphTable.Count} chars={font.characterTable.Count}");
        var g0 = font.glyphTable[0];
        Debug.Log($"glyph0: idx={g0.index} rect={g0.glyphRect.x},{g0.glyphRect.y},{g0.glyphRect.width},{g0.glyphRect.height}");
        foreach (var c in font.characterTable)
        {
            if (c.unicode == 65)
            {
                var gg = font.glyphTable[(int)c.glyphIndex];
                Debug.Log($"char A(65) -> glyph {c.glyphIndex} rect={gg.glyphRect.x},{gg.glyphRect.y},{gg.glyphRect.width},{gg.glyphRect.height}");
                break;
            }
        }
        foreach (var c in font.characterTable)
        {
            if (c.unicode == 33)
            {
                var gg = font.glyphTable[(int)c.glyphIndex];
                Debug.Log($"char !(33) -> glyph {c.glyphIndex} rect={gg.glyphRect.x},{gg.glyphRect.y},{gg.glyphRect.width},{gg.glyphRect.height}");
                var raw2 = font.atlasTexture.GetRawTextureData();
                int nz2 = 0;
                for (int yy = gg.glyphRect.y; yy < gg.glyphRect.y + gg.glyphRect.height && yy < 1024; yy++)
                    for (int xx = gg.glyphRect.x; xx < gg.glyphRect.x + gg.glyphRect.width && xx < 1024; xx++)
                        if (raw2[yy * 1024 + xx] != 0) nz2++;
                Debug.Log($"FONT ! rect density: {nz2} non-zero px (rect={gg.glyphRect.x},{gg.glyphRect.y},{gg.glyphRect.width},{gg.glyphRect.height})");
                break;
            }
        }
        var raw = font.atlasTexture.GetRawTextureData();
        Debug.Log($"atlas raw len={raw.Length} first16={System.BitConverter.ToString(System.Linq.Enumerable.Take(raw, 16).ToArray())}");
        var j = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>("Assets/Aoi/Resources/AoiSDF.asset");
        if (j != null)
        {
            Debug.Log($"AOI: name={j.name} mode={j.atlasPopulationMode} atlas={j.atlasTexture.width}x{j.atlasTexture.height} glyphs={j.glyphTable.Count} chars={j.characterTable.Count}");
            foreach (var c in j.characterTable)
            {
                if (c.unicode == 65)
                {
                    var gg = j.glyphTable[(int)c.glyphIndex];
                    Debug.Log($"AOI char A(65) -> glyph {c.glyphIndex} rect={gg.glyphRect.x},{gg.glyphRect.y},{gg.glyphRect.width},{gg.glyphRect.height}");
                    var jraw = j.atlasTexture.GetRawTextureData();
                    int nz = 0;
                    for (int yy = gg.glyphRect.y; yy < gg.glyphRect.y + gg.glyphRect.height && yy < 512; yy++)
                        for (int xx = gg.glyphRect.x; xx < gg.glyphRect.x + gg.glyphRect.width && xx < 512; xx++)
                            if (jraw[yy * 512 + xx] != 0) nz++;
                    Debug.Log($"AOI A rect density (direct, no flip): {nz} non-zero px");
                    break;
                }
            }
        }
        else Debug.LogError("AOI: not found");
    }

    [MenuItem("Build/Build Aoi")]
    public static void Build()
    {
        EnsureChineseFallbackFont();
        EnsureMonoFontAsset();
        EnsureIncludedShaders();
        EnsureAlwaysIncludedShaders();

        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        scene.name = "AoiMain";

        var go = new GameObject("AoiBootstrap");
        go.AddComponent<AoiBootstrap>();

        EditorSceneManager.SaveScene(scene, "Assets/AoiMain.unity");

        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64, new[] { GraphicsDeviceType.Direct3D11 });
        PlayerSettings.allowUnsafeCode = true;
        // Windowed, resizable player window: fullscreen startup left users with
        // a full-screen window that had no visible close button (the window
        // stays visible to mirror the hand panel on the desktop).
        PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
        PlayerSettings.resizableWindow = true;
        PlayerSettings.defaultScreenWidth = 684;
        PlayerSettings.defaultScreenHeight = 684;

        // Force the IL2CPP backend: Mono output cannot be shipped (the old
        // package was missing UnityPlayer.dll and leaked Assembly-CSharp.dll).
        // Requires the IL2CPP support module (see docs/IL2CPP_MIGRATION.md).
        PlayerSettings.SetScriptingBackend(NamedBuildTarget.Standalone, ScriptingImplementation.IL2CPP);

        var scenes = new[] { "Assets/AoiMain.unity" };
        var options = new BuildPlayerOptions
        {
            scenes = scenes,
            locationPathName = "Build/AoiVR.exe",
            target = BuildTarget.StandaloneWindows64,
            options = BuildOptions.None,
        };

        var report = BuildPipeline.BuildPlayer(options);
        if (report.summary.result == BuildResult.Succeeded)
        {
            Debug.Log("Build succeeded: " + report.summary.outputPath);
            WriteVrManifestAndBindings();
            CopyAgentRuntimeFiles();
        }
        else
            Debug.LogError("Build failed: " + report.summary.result);
    }

    // Copy the files the embedded C++ agent needs at runtime next to the player:
    // aoi_config.json.example (runtime config template) into the build root,
    // so the agent's workdir always has the template to copy from. Also copies
    // the VRChat integration skill docs (docs/vrchat-assistant/) so the agent's
    // read tool can load them at runtime.
    static void CopyAgentRuntimeFiles()
    {
        try
        {
            var buildDir = System.IO.Path.GetFullPath("Build");
            var dataDir = System.IO.Path.Combine(buildDir, "AoiVR_Data");
            // Same source repo as the Unity project: <project>/../agent-cpp/
            // (resolved from Application.dataPath, independent of the cwd).
            var configExample = System.IO.Path.GetFullPath(
                System.IO.Path.Combine(Application.dataPath, "..", "..", "agent-cpp", "aoi_config.json.example"));
            if (!System.IO.File.Exists(configExample))
            {
                Debug.LogWarning("agent-cpp/aoi_config.json.example not found at " + configExample);
                return;
            }
            System.IO.Directory.CreateDirectory(dataDir);
            System.IO.File.Copy(configExample, System.IO.Path.Combine(buildDir, "aoi_config.json.example"), true);
            Debug.Log("Copied aoi_config.json.example into build (" + buildDir + ")");

            // VRChat skill docs: <project>/../docs/vrchat-assistant/ -> Build/docs/vrchat-assistant/
            var skillSrc = System.IO.Path.GetFullPath(
                System.IO.Path.Combine(Application.dataPath, "..", "..", "docs", "vrchat-assistant"));
            if (System.IO.Directory.Exists(skillSrc))
            {
                var skillDst = System.IO.Path.Combine(buildDir, "docs", "vrchat-assistant");
                CopyDirectory(skillSrc, skillDst);
                Debug.Log("Copied vrchat-assistant skill docs into build (" + skillDst + ")");
            }
            else
            {
                Debug.LogWarning("docs/vrchat-assistant not found at " + skillSrc);
            }
        }
        catch (System.Exception e)
        {
            Debug.LogError("CopyAgentRuntimeFiles failed: " + e.Message);
        }
    }

    static void CopyDirectory(string src, string dst)
    {
        System.IO.Directory.CreateDirectory(dst);
        foreach (var f in System.IO.Directory.GetFiles(src))
            System.IO.File.Copy(f, System.IO.Path.Combine(dst, System.IO.Path.GetFileName(f)), true);
        foreach (var d in System.IO.Directory.GetDirectories(src))
            CopyDirectory(d, System.IO.Path.Combine(dst, System.IO.Path.GetFileName(d)));
    }

    // Generates app.vrmanifest (with default_bindings) and copies the action
    // manifest + binding JSONs next to it so SteamVR can register the app and
    // show it in Controller Bindings management.
    static void WriteVrManifestAndBindings()
    {
        try
        {
            var buildDir = System.IO.Path.GetFullPath("Build");
            var manifestPath = System.IO.Path.Combine(buildDir, "app.vrmanifest");
            System.IO.File.WriteAllText(manifestPath, @"{
  ""source"": ""builtin"",
  ""applications"": [
    {
      ""app_key"": ""aoi.hand.panel"",
      ""launch_type"": ""binary"",
      ""binary_path_windows"": ""AoiVR.exe"",
      ""is_dashboard_overlay"": false,
      ""can_composite"": true,
      ""strings"": {
        ""en_us"": {
          ""name"": ""Aoi Hand Panel"",
          ""description"": ""AI-powered VR hand panel assistant""
        }
      }
    }
  ],
  ""default_bindings"": [
    { ""controller_type"": ""knuckles"", ""binding_url"": ""bindings_knuckles.json"" },
    { ""controller_type"": ""vive_controller"", ""binding_url"": ""bindings_vive.json"" },
    { ""controller_type"": ""oculus_touch"", ""binding_url"": ""bindings_oculus.json"" },
    { ""controller_type"": ""holographic_controller"", ""binding_url"": ""bindings_holographic.json"" },
    { ""controller_type"": ""index_controller"", ""binding_url"": ""bindings_index.json"" },
    { ""controller_type"": ""pico_controller"", ""binding_url"": ""bindings_pico_controller.json"" }
  ]
}
");
            var sa = new System.IO.DirectoryInfo("Assets/StreamingAssets");
            if (sa.Exists)
            {
                foreach (var f in sa.GetFiles("*.json"))
                    System.IO.File.Copy(f.FullName, System.IO.Path.Combine(buildDir, f.Name), true);
            }
            Debug.Log("app.vrmanifest + bindings written to " + buildDir);
        }
        catch (System.Exception e)
        {
            Debug.LogError("WriteVrManifestAndBindings failed: " + e.Message);
        }
    }

    static void EnsureBakedFont()
    {
        string path = "Assets/Aoi/Resources/AoiSDF.asset";
        var existing = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>(path);
        if (existing != null)
        {
            var tex = existing.atlasTexture;
            bool corrupt = (tex != null && tex.isReadable && tex.GetRawTextureData().Length == 0 && existing.glyphTable != null && existing.glyphTable.Count > 0);
            bool noMaterial = existing.material == null;
            if (corrupt || noMaterial)
            {
                Debug.LogWarning($"AoiSDF.asset invalid (corrupt={corrupt} noMaterial={noMaterial}) - deleting, will recreate");
                AssetDatabase.DeleteAsset(path);
            }
            else
            {
                return;
            }
        }
        try
        {
            var ttfPath = System.IO.Path.Combine(Application.dataPath, "TextMesh Pro", "Fonts", "LiberationSans.ttf");
            var sdf = TMP_FontAsset.CreateFontAsset(ttfPath, 0, 64, 9, UnityEngine.TextCore.LowLevel.GlyphRenderMode.SDFAA, 512, 512);
            if (sdf == null) { Debug.LogError("CreateFontAsset failed"); return; }
            var unicodes = new List<uint>();
            for (uint c = 32; c < 127; c++) unicodes.Add(c);
            var missing = new uint[0];
            bool added = sdf.TryAddCharacters(unicodes.ToArray(), out missing);
            var texInfo = sdf.atlasTexture != null ? sdf.atlasTexture.width + "x" + sdf.atlasTexture.height : "NULL";
            var glyphCount = sdf.glyphTable != null ? sdf.glyphTable.Count : -1;
            var rawLen = sdf.atlasTexture != null ? sdf.atlasTexture.GetRawTextureData().Length : 0;
            if (glyphCount <= 0 || rawLen == 0)
            {
                Debug.LogError($"Baked AoiSDF invalid (glyphs={glyphCount} tex={texInfo} rawLen={rawLen}) - discarding, will fall back to LiberationSans SDF");
                return;
            }
            sdf.atlasPopulationMode = TMPro.AtlasPopulationMode.Static;
            sdf.name = "AoiSDF";
            AssetDatabase.CreateAsset(sdf, path);
            if (sdf.material != null)
            {
                sdf.material.name = sdf.name + " Atlas Material";
                AssetDatabase.AddObjectToAsset(sdf.material, sdf);
            }
            AssetDatabase.AddObjectToAsset(sdf.atlasTexture, sdf);
            AssetDatabase.SaveAssets();
            var disk = File.ReadAllText(path);
            if (!disk.Contains("_typelessdata"))
            {
                Debug.LogError("AoiSDF atlas data missing on disk - deleting, will fall back");
                AssetDatabase.DeleteAsset(path);
                return;
            }
            var missingLen = missing != null ? missing.Length : -1;
            Debug.Log($"Baked AoiSDF font asset created: {texInfo} rawLen={rawLen} glyphs={glyphCount} missing={missingLen} added={added} mode={sdf.atlasPopulationMode} diskHasData=true");
        }
        catch (System.Exception e)
        {
            if (File.Exists(path)) File.Delete(path);
            Debug.LogError("EnsureBakedFont failed (removed broken AoiSDF): " + e.Message + "\n" + e.StackTrace);
        }
    }

    static void EnsureChineseFallbackFont()
    {
        string path = "Assets/Aoi/Resources/AoiCN.asset";
        var existing = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>(path);
        if (existing != null)
        {
            if (existing.material == null)
            {
                Debug.LogWarning("AoiCN.asset missing material - deleting, will recreate");
                AssetDatabase.DeleteAsset(path);
            }
            else
            {
                // Ensure the fallback link is wired even if the asset already exists.
                var mainFont = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>("Assets/TextMesh Pro/Resources/Fonts & Materials/LiberationSans SDF.asset");
                if (mainFont != null && !mainFont.fallbackFontAssetTable.Contains(existing))
                {
                    mainFont.fallbackFontAssetTable.Add(existing);
                    EditorUtility.SetDirty(mainFont);
                    AssetDatabase.SaveAssets();
                    Debug.Log("AoiCN added to LiberationSans SDF fallback table (existing asset)");
                }
                return;
            }
        }
        try
        {
            var cnFont = AssetDatabase.LoadAssetAtPath<Font>("Assets/Aoi/Resources/NotoSansCJKsc-Regular.otf");
            if (cnFont == null) { Debug.LogError("Chinese font creation failed (NotoSansCJKsc-Regular.otf not found)"); return; }
            var cn = TMP_FontAsset.CreateFontAsset(cnFont);
            cn.name = "AoiCN";
            var commonChars = "的一是了我不人在他有这个上们来到时大地为子中你说生国年着就那和要她出也得里后自以会家可下而过天去能对小多然于心学么之都好看起发当没成只如事把还用第样道想作种开美总从无情己面最女但现前些所同日手又行意动方期它头经长儿回位分爱老因很给名法间斯知世什两次使身者被高已亲其进此话常与活正感";
            var unicodes = new List<uint>();
            foreach (var ch in commonChars)
                if (!unicodes.Contains(ch)) unicodes.Add(ch);
            bool preAdded = cn.TryAddCharacters(unicodes.ToArray());
            AssetDatabase.CreateAsset(cn, path);
            if (cn.material != null)
            {
                cn.material.name = cn.name + " Atlas Material";
                AssetDatabase.AddObjectToAsset(cn.material, cn);
            }
            if (cn.atlasTexture != null)
                AssetDatabase.AddObjectToAsset(cn.atlasTexture, cn);
            AssetDatabase.SaveAssets();
            Debug.Log($"Chinese fallback font created: mode={cn.atlasPopulationMode} glyphs={cn.glyphTable.Count} preAdded={preAdded}");

            var mainFont = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>("Assets/TextMesh Pro/Resources/Fonts & Materials/LiberationSans SDF.asset");
            if (mainFont != null && !mainFont.fallbackFontAssetTable.Contains(cn))
            {
                mainFont.fallbackFontAssetTable.Add(cn);
                EditorUtility.SetDirty(mainFont);
                AssetDatabase.SaveAssets();
                Debug.Log("AoiCN added to LiberationSans SDF fallback table");
            }
        }
        catch (System.Exception e)
        {
            if (File.Exists(path)) File.Delete(path);
            Debug.LogError("EnsureChineseFallbackFont failed: " + e.Message + "\n" + e.StackTrace);
        }
    }

    // JetBrains Mono-based monospace font for the terminal-style texts (logo,
    // status, chips, hints). Dynamic atlas; CJK glyphs fall back to AoiCN (Noto).
    static void EnsureMonoFontAsset()
    {
        string path = "Assets/Aoi/Resources/AoiMono.asset";
        var existing = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>(path);
        if (existing != null)
        {
            if (existing.material == null)
            {
                Debug.LogWarning("AoiMono.asset missing material - deleting, will recreate");
                AssetDatabase.DeleteAsset(path);
            }
            else
            {
                EnsureMonoFallback(existing);
                return;
            }
        }
        try
        {
            var monoFont = AssetDatabase.LoadAssetAtPath<Font>("Assets/Aoi/Resources/JetBrainsMono-Regular.ttf");
            if (monoFont == null) { Debug.LogError("JetBrainsMono font creation failed (JetBrainsMono-Regular.ttf not found)"); return; }
            var mono = TMP_FontAsset.CreateFontAsset(monoFont);
            mono.name = "AoiMono";
            AssetDatabase.CreateAsset(mono, path);
            if (mono.material != null)
            {
                mono.material.name = mono.name + " Atlas Material";
                AssetDatabase.AddObjectToAsset(mono.material, mono);
            }
            if (mono.atlasTexture != null)
                AssetDatabase.AddObjectToAsset(mono.atlasTexture, mono);
            AssetDatabase.SaveAssets();
            Debug.Log($"Mono font created: mode={mono.atlasPopulationMode} glyphs={mono.glyphTable.Count}");
            EnsureMonoFallback(mono);
        }
        catch (System.Exception e)
        {
            if (File.Exists(path)) File.Delete(path);
            Debug.LogError("EnsureMonoFontAsset failed: " + e.Message + "\n" + e.StackTrace);
        }
    }

    static void EnsureMonoFallback(TMP_FontAsset mono)
    {
        var cn = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>("Assets/Aoi/Resources/AoiCN.asset");
        if (cn == null) return;
        if (mono.fallbackFontAssetTable == null)
            mono.fallbackFontAssetTable = new System.Collections.Generic.List<TMP_FontAsset>();
        if (!mono.fallbackFontAssetTable.Contains(cn))
        {
            mono.fallbackFontAssetTable.Add(cn);
            EditorUtility.SetDirty(mono);
            AssetDatabase.SaveAssets();
            Debug.Log("AoiCN added to AoiMono fallback table");
        }
    }

    static void EnsureIncludedShaders()
    {
        string dir = "Assets/Aoi/Resources";
        if (!AssetDatabase.IsValidFolder("Assets/Aoi"))
            AssetDatabase.CreateFolder("Assets", "Jarvis");
        if (!AssetDatabase.IsValidFolder(dir))
            AssetDatabase.CreateFolder("Assets/Aoi", "Resources");

        string[] shaderNames = { "UI/Default", "Sprites/Default", "Unlit/Color", "TextMeshPro/Distance Field", "TextMeshPro/Distance Field Overlay", "TextMeshPro/Mobile/Distance Field" };
        foreach (var s in shaderNames)
        {
            var path = dir + "/" + s.Replace("/", "_").Replace(" ", "_") + ".mat";
            if (File.Exists(path)) continue;
            var shader = Shader.Find(s);
            if (shader == null)
            {
                Debug.LogWarning("Shader not found for inclusion: " + s);
                continue;
            }
            var mat = new Material(shader);
            AssetDatabase.CreateAsset(mat, path);
            Debug.Log("Created shader-reference material: " + path);
        }
        AssetDatabase.SaveAssets();
    }

    static void EnsureAlwaysIncludedShaders()
    {
        string path = "ProjectSettings/GraphicsSettings.asset";
        if (!File.Exists(path))
        {
            Debug.LogError("GraphicsSettings.asset not found");
            return;
        }
        string yaml = File.ReadAllText(path);
        string[] guids = {
            "68e6db2ebdc24f95958faec2be5558d6", // TMP_SDF
            "dd89cf5b9246416f84610a006f916af7", // TMP_SDF Overlay
            "fe393ace9b354375a9cb14cdbbc28be4", // TMP_SDF-Mobile
            "cf81c85f95fe47e1a27f6ae460cf182c", // TMP_Sprite
        };
        bool changed = false;
        foreach (var g in guids)
        {
            if (yaml.Contains(g)) continue;
            int idx = yaml.IndexOf("m_AlwaysIncludedShaders:");
            if (idx < 0) { Debug.LogError("m_AlwaysIncludedShaders not found in GraphicsSettings.asset"); return; }
            int insertAt = yaml.IndexOf("\n", idx) + 1;
            yaml = yaml.Insert(insertAt, "    - {fileID: 4800000, guid: " + g + ", type: 3}\n");
            changed = true;
            Debug.Log("AlwaysIncludedShaders += " + g);
        }
        if (changed)
        {
            File.WriteAllText(path, yaml);
            Debug.Log("GraphicsSettings.asset updated");
        }
    }
}
