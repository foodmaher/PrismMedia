using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace PrismMediaClient
{
    internal static class AccessoryBridgeService
    {
        private const string GeneratedModName = "PrismMedia_Generated_Display";

        private sealed class Accessory
        {
            public string Name;
            public string Definition;
            public string InteriorModel;
            public string ExteriorModel;
            public string UiTexture;
        }

        private sealed class AdapterGroup
        {
            public string Id;
            public string Name;
            public string InteriorModel;
            public string ExteriorModel;
            public string UiTexture;
            public string MediaTobj;
            public uint Width;
            public uint Height;
            public readonly List<Accessory> Definitions = new List<Accessory>();
        }

        private sealed class ModCandidate
        {
            public string Locator;
            public string DisplayName;
            public int ActiveScore;
            public readonly List<Accessory> Accessories = new List<Accessory>();
        }

        private sealed class ArchiveView : IDisposable
        {
            private readonly string directoryRoot;
            private readonly Stream owner;
            private readonly ZipArchive archive;

            private ArchiveView(string root) { directoryRoot = root; }
            private ArchiveView(Stream stream, ZipArchive zip)
            {
                owner = stream;
                archive = zip;
            }

            public static ArchiveView Open(string locator)
            {
                string[] parts = locator.Split(new[] { '|' }, 2);
                if (parts.Length == 1 && Directory.Exists(parts[0]))
                    return new ArchiveView(Path.GetFullPath(parts[0]));

                FileStream file = File.Open(parts[0], FileMode.Open,
                    FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                ZipArchive outer = new ZipArchive(file, ZipArchiveMode.Read, false);
                if (parts.Length == 1)
                    return new ArchiveView(file, outer);

                ZipArchiveEntry nested = FindEntry(outer, parts[1]);
                if (nested == null)
                {
                    outer.Dispose();
                    file.Dispose();
                    throw new InvalidDataException("Nested mod archive was not found.");
                }
                MemoryStream memory = new MemoryStream();
                using (Stream input = nested.Open())
                    input.CopyTo(memory);
                memory.Position = 0;
                outer.Dispose();
                file.Dispose();
                return new ArchiveView(memory,
                    new ZipArchive(memory, ZipArchiveMode.Read, true));
            }

            public bool Exists(string path)
            {
                string wanted = Normalize(path).TrimStart('/');
                if (directoryRoot != null)
                    return File.Exists(PhysicalPath(wanted));
                return FindEntry(archive, wanted) != null;
            }

            public byte[] Read(string path)
            {
                string wanted = Normalize(path).TrimStart('/');
                if (directoryRoot != null)
                    return File.ReadAllBytes(PhysicalPath(wanted));
                ZipArchiveEntry entry = FindEntry(archive, wanted);
                if (entry == null)
                    throw new FileNotFoundException("Mod entry was not found.", path);
                using (Stream input = entry.Open())
                using (MemoryStream output = new MemoryStream())
                {
                    input.CopyTo(output);
                    return output.ToArray();
                }
            }

            public IEnumerable<string> Entries
            {
                get
                {
                    if (directoryRoot != null)
                    {
                        return Directory.EnumerateFiles(directoryRoot, "*",
                            SearchOption.AllDirectories).Select(path => Normalize(
                                path.Substring(directoryRoot.Length).TrimStart(
                                    Path.DirectorySeparatorChar,
                                    Path.AltDirectorySeparatorChar)));
                    }
                    return archive.Entries.Select(entry => Normalize(entry.FullName));
                }
            }

            private string PhysicalPath(string virtualPath)
            {
                string relative = virtualPath.Replace('/', Path.DirectorySeparatorChar);
                string full = Path.GetFullPath(Path.Combine(directoryRoot, relative));
                string prefix = directoryRoot.TrimEnd(
                    Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) +
                    Path.DirectorySeparatorChar;
                if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Invalid path inside directory mod.");
                return full;
            }

            public void Dispose()
            {
                if (archive != null) archive.Dispose();
                if (owner != null) owner.Dispose();
            }
        }

        private static readonly Regex MaterialPathRegex = new Regex(
            @"/automat/[0-9a-fA-F]{2}/[0-9a-fA-F]{16}\.mat",
            RegexOptions.Compiled);

        public static bool TryRun(string[] args)
        {
            string mode = GetArgument(args, "--bridge-mode");
            if (string.IsNullOrWhiteSpace(mode))
                return false;
            string result = GetArgument(args, "--result");
            try
            {
                if (!string.Equals(mode, "auto", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Unknown accessory bridge mode.");
                GenerateAutomatically(args, result);
            }
            catch (Exception error)
            {
                WriteError(result, error.Message);
            }
            return true;
        }

        private static void GenerateAutomatically(string[] args, string resultPath)
        {
            string gameHome = GetArgument(args, "--game-home");
            string gameExe = GetArgument(args, "--game-exe");
            if (string.IsNullOrWhiteSpace(gameHome) ||
                string.IsNullOrWhiteSpace(gameExe))
                throw new InvalidDataException("The game folders could not be located.");

            string gameLog = ReadTextIfPresent(Path.Combine(gameHome, "game.log.txt"));
            List<ModCandidate> candidates = new List<ModCandidate>();
            foreach (string locator in FindModLocators(gameHome, gameExe))
            {
                ModCandidate candidate = InspectCandidate(locator, gameLog);
                if (candidate != null)
                    candidates.Add(candidate);
            }
            ModCandidate selected = SelectCandidate(candidates);
            if (selected == null)
            {
                WriteError(resultPath,
                    "No active ZIP-compatible TJS display mod was found in the local mod folder or any Steam Workshop library.");
                return;
            }

            List<AdapterGroup> groups = BuildGroups(selected.Accessories);
            if (groups.Count == 0)
                throw new InvalidDataException(
                    "The selected TJS package has no compatible display definitions.");

            string modRoot = Path.Combine(gameHome, "mod", GeneratedModName);
            string fingerprint = SourceFingerprint(selected, groups);
            bool generated = !string.Equals(
                ReadMarkerValue(Path.Combine(modRoot, ".prismmedia.ini"), "Fingerprint"),
                fingerprint, StringComparison.OrdinalIgnoreCase);
            if (generated)
                GenerateMod(selected, groups, gameHome, modRoot, fingerprint);

            bool active = GeneratedModActive(gameLog, modRoot);
            StringBuilder output = new StringBuilder();
            output.AppendLine("[Result]");
            output.AppendLine("Status=ok");
            output.AppendLine("Message=" + IniValue(BuildResultMessage(
                selected, groups.Count, generated, active)));
            output.AppendLine("Source=" + IniValue(selected.DisplayName));
            output.AppendLine("SourceLocator=" + IniValue(selected.Locator));
            output.AppendLine("ModPath=" + IniValue(modRoot));
            output.AppendLine("Count=" + groups.Count.ToString(CultureInfo.InvariantCulture));
            output.AppendLine("Generated=" + (generated ? "1" : "0"));
            output.AppendLine("Active=" + (active ? "1" : "0"));
            output.AppendLine("RestartRequired=" + (generated ? "1" : "0"));
            for (int index = 0; index < groups.Count; ++index)
            {
                AdapterGroup group = groups[index];
                output.AppendLine();
                output.AppendLine("[Adapter" + index.ToString(CultureInfo.InvariantCulture) + "]");
                output.AppendLine("Id=" + group.Id);
                output.AppendLine("Name=" + IniValue(group.Name));
                output.AppendLine("UiTexture=" + IniValue(group.UiTexture));
                output.AppendLine("MediaTobj=" + IniValue(group.MediaTobj));
                output.AppendLine("Width=" + group.Width.ToString(CultureInfo.InvariantCulture));
                output.AppendLine("Height=" + group.Height.ToString(CultureInfo.InvariantCulture));
                output.AppendLine("Definitions=" + group.Definitions.Count.ToString(CultureInfo.InvariantCulture));
            }
            WriteResult(resultPath, output.ToString());
        }

        private static ModCandidate InspectCandidate(string locator, string gameLog)
        {
            string normalizedLocator = Normalize(locator);
            if (normalizedLocator.IndexOf("/" + GeneratedModName + "/",
                    StringComparison.OrdinalIgnoreCase) >= 0 ||
                normalizedLocator.EndsWith("/" + GeneratedModName,
                    StringComparison.OrdinalIgnoreCase))
                return null;
            try
            {
                using (ArchiveView archive = ArchiveView.Open(locator))
                {
                    List<string> entries = archive.Entries.ToList();
                    string manifest = entries.FirstOrDefault(path => string.Equals(
                        path.TrimStart('/'), "manifest.sii",
                        StringComparison.OrdinalIgnoreCase));
                    string manifestText = manifest == null ? "" :
                        DecodeText(archive.Read(manifest));
                    string displayName = ReadQuoted(manifestText, "display_name") ??
                        Path.GetFileNameWithoutExtension(locator.Split('|')[0]);
                    bool tjsEvidence = locator.IndexOf("tjs",
                        StringComparison.OrdinalIgnoreCase) >= 0 ||
                        manifestText.IndexOf("tjs",
                        StringComparison.OrdinalIgnoreCase) >= 0;
                    ModCandidate result = new ModCandidate
                    {
                        Locator = locator,
                        DisplayName = displayName
                    };
                    foreach (string path in entries.Where(path =>
                        path.EndsWith(".sii", StringComparison.OrdinalIgnoreCase) &&
                        path.StartsWith("def/", StringComparison.OrdinalIgnoreCase)))
                    {
                        byte[] bytes;
                        try { bytes = archive.Read(path); }
                        catch { continue; }
                        if (bytes.Length > 2 * 1024 * 1024)
                            continue;
                        string definition = DecodeText(bytes);
                        Accessory accessory = ParseAccessory(definition);
                        if (accessory == null)
                            continue;
                        accessory.Definition = Normalize(path).TrimStart('/');
                        result.Accessories.Add(accessory);
                        tjsEvidence = tjsEvidence || definition.IndexOf("tjs",
                            StringComparison.OrdinalIgnoreCase) >= 0;
                    }
                    if (!tjsEvidence || result.Accessories.Count == 0)
                        return null;
                    result.ActiveScore = ActiveScore(result, gameLog);
                    return result;
                }
            }
            catch { return null; }
        }

        private static ModCandidate SelectCandidate(List<ModCandidate> candidates)
        {
            if (candidates.Count == 0) return null;
            if (candidates.Count == 1) return candidates[0];
            List<ModCandidate> ranked = candidates
                .OrderByDescending(candidate => candidate.ActiveScore)
                .ThenByDescending(candidate => candidate.Accessories.Count).ToList();
            if (ranked[0].ActiveScore <= 0) return null;
            if (ranked.Count > 1 && ranked[0].ActiveScore == ranked[1].ActiveScore &&
                ranked[0].Accessories.Count == ranked[1].Accessories.Count)
                return null;
            return ranked[0];
        }

        private static int ActiveScore(ModCandidate candidate, string gameLog)
        {
            if (string.IsNullOrWhiteSpace(gameLog)) return 0;
            string log = Normalize(gameLog).ToLowerInvariant();
            string source = Normalize(candidate.Locator.Split('|')[0]).ToLowerInvariant();
            int score = 0;
            if (log.Contains(source)) score += 1000;
            string fileName = Path.GetFileName(source);
            if (!string.IsNullOrWhiteSpace(fileName) && log.Contains(fileName)) score += 200;
            Match workshop = Regex.Match(source,
                @"/workshop/content/(?:227300|270880)/([0-9]+)",
                RegexOptions.IgnoreCase);
            if (workshop.Success && log.Contains(workshop.Groups[1].Value)) score += 300;
            string display = (candidate.DisplayName ?? "").ToLowerInvariant();
            if (display.Length >= 4 && log.Contains(display)) score += 100;
            return score;
        }

        private static List<AdapterGroup> BuildGroups(List<Accessory> accessories)
        {
            Dictionary<string, AdapterGroup> groups = new Dictionary<string, AdapterGroup>(
                StringComparer.OrdinalIgnoreCase);
            foreach (Accessory accessory in accessories)
            {
                string key = Normalize(accessory.InteriorModel).ToLowerInvariant() + "|" +
                    Normalize(accessory.ExteriorModel).ToLowerInvariant() + "|" +
                    Normalize(accessory.UiTexture).ToLowerInvariant();
                AdapterGroup group;
                if (!groups.TryGetValue(key, out group))
                {
                    string id = ShortHash(key, 8);
                    group = new AdapterGroup
                    {
                        Id = id,
                        InteriorModel = accessory.InteriorModel,
                        ExteriorModel = accessory.ExteriorModel,
                        UiTexture = accessory.UiTexture,
                        MediaTobj = "/home/PrismTextureStreamer/adapter_" + id + ".tobj"
                    };
                    groups.Add(key, group);
                }
                group.Definitions.Add(accessory);
            }

            HashSet<ulong> dimensions = new HashSet<ulong>();
            foreach (AdapterGroup group in groups.Values.OrderBy(group => group.Id))
            {
                Accessory common = group.Definitions
                    .GroupBy(item => item.Name ?? "Display", StringComparer.OrdinalIgnoreCase)
                    .OrderByDescending(items => items.Count()).First().First();
                group.Name = (common.Name ?? "TJS display") + " - " +
                    Path.GetFileNameWithoutExtension(group.InteriorModel);
                byte[] hash = HashBytes(group.Id);
                uint slot = (uint)((hash[8] | (hash[9] << 8)) % 4096);
                for (uint attempt = 0; attempt < 4096; ++attempt)
                {
                    uint selected = (slot + attempt) % 4096;
                    uint width = 132U + (selected % 64U) * 4U;
                    uint height = 2052U + (selected / 64U) * 4U;
                    ulong packed = ((ulong)width << 32) | height;
                    if (dimensions.Add(packed))
                    {
                        group.Width = width;
                        group.Height = height;
                        break;
                    }
                }
            }
            return groups.Values.OrderBy(group => group.Name,
                StringComparer.OrdinalIgnoreCase).ToList();
        }

        private static void GenerateMod(ModCandidate source,
            List<AdapterGroup> groups, string gameHome, string modRoot,
            string fingerprint)
        {
            string modParent = Path.GetFullPath(Path.Combine(gameHome, "mod"));
            string expected = Path.GetFullPath(Path.Combine(modParent, GeneratedModName));
            if (!string.Equals(expected, Path.GetFullPath(modRoot),
                    StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Generated mod path validation failed.");
            Directory.CreateDirectory(modParent);
            string stage = expected + ".tmp";
            if (Directory.Exists(stage)) Directory.Delete(stage, true);
            Directory.CreateDirectory(stage);
            try
            {
                using (ArchiveView archive = ArchiveView.Open(source.Locator))
                {
                    foreach (AdapterGroup group in groups)
                    {
                        Dictionary<string, string> modelMap =
                            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                        foreach (string model in new[] { group.InteriorModel, group.ExteriorModel })
                        {
                            if (string.IsNullOrWhiteSpace(model) || modelMap.ContainsKey(model)) continue;
                            modelMap[model] = CopyPrivateModel(archive, stage, model,
                                group.UiTexture, group.MediaTobj, group.Id);
                        }
                        foreach (Accessory accessory in group.Definitions)
                        {
                            string definition = DecodeText(archive.Read(accessory.Definition));
                            foreach (KeyValuePair<string, string> pair in modelMap)
                                definition = ReplaceQuotedPath(definition, pair.Key, pair.Value);
                            WriteBytes(stage, accessory.Definition,
                                new UTF8Encoding(false).GetBytes(definition));
                        }
                    }
                }
                WriteManifest(stage);
                File.WriteAllText(Path.Combine(stage, ".prismmedia.ini"),
                    "[Adapter]\r\nFingerprint=" + fingerprint +
                    "\r\nSource=" + IniValue(source.Locator) +
                    "\r\nGroups=" + groups.Count.ToString(CultureInfo.InvariantCulture) +
                    "\r\n", new UTF8Encoding(false));
                if (Directory.Exists(expected)) Directory.Delete(expected, true);
                Directory.Move(stage, expected);
            }
            catch
            {
                if (Directory.Exists(stage)) Directory.Delete(stage, true);
                throw;
            }
        }

        private static string CopyPrivateModel(ArchiveView archive,
            string modRoot, string modelPath, string uiTexture,
            string prismTexture, string groupId)
        {
            string normalized = Normalize(modelPath).TrimStart('/');
            string extension = Path.GetExtension(normalized);
            if (string.IsNullOrWhiteSpace(extension))
                throw new InvalidDataException("A display model has no file extension.");
            string stem = normalized.Substring(0, normalized.Length - extension.Length);
            string privateStem = "vehicle/prismmedia/" + groupId + "/" +
                SafeName(Path.GetFileName(stem)) + "_" + ShortHash(normalized, 4);
            byte[] pmd = archive.Read(normalized);
            string pmdText = Encoding.ASCII.GetString(pmd);
            int patched = 0;
            foreach (string originalMaterial in MaterialPathRegex.Matches(pmdText)
                .Cast<Match>().Select(match => match.Value)
                .Distinct(StringComparer.OrdinalIgnoreCase))
            {
                string materialPath = Normalize(originalMaterial).TrimStart('/');
                if (!archive.Exists(materialPath)) continue;
                string material = DecodeText(archive.Read(materialPath));
                if (material.IndexOf(uiTexture, StringComparison.OrdinalIgnoreCase) < 0) continue;
                string privateMaterial = PrivateMaterialPath(
                    groupId + "|" + normalized + "|" + originalMaterial);
                patched += ReplaceBytes(pmd, Encoding.ASCII.GetBytes(originalMaterial),
                    Encoding.ASCII.GetBytes(privateMaterial));
                material = ReplaceQuotedPath(material, uiTexture, prismTexture);
                WriteBytes(modRoot, privateMaterial.TrimStart('/'),
                    new UTF8Encoding(false).GetBytes(material));
            }
            if (patched == 0)
                throw new InvalidDataException(
                    "The model material that uses the GPS picture could not be identified: " + modelPath);
            WriteBytes(modRoot, privateStem + ".pmd", pmd);
            foreach (string suffix in new[] { ".pmg", ".pma", ".pmc" })
            {
                string sourcePath = stem + suffix;
                if (archive.Exists(sourcePath))
                    WriteBytes(modRoot, privateStem + suffix, archive.Read(sourcePath));
            }
            return "/" + privateStem + ".pmd";
        }

        private static Accessory ParseAccessory(string definition)
        {
            string interior = ReadQuoted(definition, "interior_model");
            if (string.IsNullOrWhiteSpace(interior)) interior = ReadQuoted(definition, "model");
            string exterior = ReadQuoted(definition, "exterior_model");
            string texture = ReadQuoted(definition, "ui_drawable_texture_path");
            if (string.IsNullOrWhiteSpace(interior) || string.IsNullOrWhiteSpace(texture))
                return null;
            return new Accessory
            {
                Name = ReadQuoted(definition, "name") ?? Path.GetFileNameWithoutExtension(interior),
                InteriorModel = Normalize(interior),
                ExteriorModel = string.IsNullOrWhiteSpace(exterior) ? Normalize(interior) : Normalize(exterior),
                UiTexture = Normalize(texture)
            };
        }

        private static List<string> FindModLocators(string gameHome, string gameExe)
        {
            HashSet<string> roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            roots.Add(Path.Combine(gameHome, "mod"));
            foreach (string workshop in SteamWorkshopRoots(gameExe)) roots.Add(workshop);
            HashSet<string> locators = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string root in roots)
            {
                if (!Directory.Exists(root)) continue;
                try
                {
                    foreach (string file in Directory.EnumerateFiles(root, "*",
                        SearchOption.AllDirectories).Where(path =>
                            path.EndsWith(".scs", StringComparison.OrdinalIgnoreCase) ||
                            path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)))
                        AddArchiveLocators(locators, file);
                    foreach (string folder in Directory.EnumerateDirectories(root, "*",
                        SearchOption.TopDirectoryOnly))
                        locators.Add(folder);
                }
                catch { }
            }
            return locators.ToList();
        }

        private static void AddArchiveLocators(HashSet<string> locators, string file)
        {
            locators.Add(file);
            try
            {
                using (FileStream stream = File.Open(file, FileMode.Open,
                    FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (ZipArchive zip = new ZipArchive(stream, ZipArchiveMode.Read))
                {
                    foreach (ZipArchiveEntry entry in zip.Entries)
                        if (entry.FullName.EndsWith(".scs", StringComparison.OrdinalIgnoreCase) ||
                            entry.FullName.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                            locators.Add(file + "|" + entry.FullName);
                }
            }
            catch { }
        }

        private static IEnumerable<string> SteamWorkshopRoots(string gameExe)
        {
            HashSet<string> steamRoots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                DirectoryInfo directory = new FileInfo(gameExe).Directory;
                while (directory != null && !string.Equals(directory.Name,
                    "steamapps", StringComparison.OrdinalIgnoreCase)) directory = directory.Parent;
                if (directory != null && directory.Parent != null) steamRoots.Add(directory.Parent.FullName);
            }
            catch { }
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                {
                    string steamPath = key == null ? null : key.GetValue("SteamPath") as string;
                    if (!string.IsNullOrWhiteSpace(steamPath))
                        steamRoots.Add(steamPath.Replace('/', Path.DirectorySeparatorChar));
                }
            }
            catch { }
            foreach (string steamRoot in steamRoots.ToList())
            {
                string libraries = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
                try
                {
                    string text = File.ReadAllText(libraries);
                    foreach (Match match in Regex.Matches(text,
                        @"""path""\s+""([^""]+)""", RegexOptions.IgnoreCase))
                        steamRoots.Add(match.Groups[1].Value.Replace(@"\\", @"\"));
                }
                catch { }
            }
            foreach (string root in steamRoots)
            {
                yield return Path.Combine(root, "steamapps", "workshop", "content", "227300");
                yield return Path.Combine(root, "steamapps", "workshop", "content", "270880");
            }
        }

        private static bool GeneratedModActive(string gameLog, string modRoot)
        {
            if (string.IsNullOrWhiteSpace(gameLog)) return false;
            string log = Normalize(gameLog).ToLowerInvariant();
            return log.Contains("prismmedia generated display") ||
                log.Contains(Normalize(modRoot).ToLowerInvariant()) ||
                log.Contains(GeneratedModName.ToLowerInvariant());
        }

        private static string SourceFingerprint(ModCandidate source, List<AdapterGroup> groups)
        {
            StringBuilder value = new StringBuilder("bridge-schema-2|");
            string physical = source.Locator.Split('|')[0];
            value.Append(Normalize(Path.GetFullPath(physical)).ToLowerInvariant());
            try
            {
                FileInfo file = new FileInfo(physical);
                if (file.Exists)
                    value.Append('|').Append(file.Length).Append('|').Append(file.LastWriteTimeUtc.Ticks);
                else if (Directory.Exists(physical))
                {
                    long newest = Directory.EnumerateFiles(physical, "*", SearchOption.AllDirectories)
                        .Select(path => File.GetLastWriteTimeUtc(path).Ticks).DefaultIfEmpty(0).Max();
                    value.Append('|').Append(newest);
                }
            }
            catch { }
            foreach (AdapterGroup group in groups.OrderBy(group => group.Id))
            {
                value.Append('|').Append(group.Id).Append('|').Append(group.Definitions.Count);
                foreach (Accessory definition in group.Definitions
                    .OrderBy(item => item.Definition, StringComparer.OrdinalIgnoreCase))
                    value.Append('|').Append(definition.Definition.ToLowerInvariant());
            }
            return ShortHash(value.ToString(), 16);
        }

        private static string BuildResultMessage(ModCandidate source,
            int count, bool generated, bool active)
        {
            if (!active)
                return "Generated " + count.ToString(CultureInfo.InvariantCulture) +
                    " private display adapters from " + source.DisplayName +
                    ". Enable 'PrismMedia Generated Display' above the source mod in Mod Manager, then restart the game.";
            if (generated)
                return "Updated " + count.ToString(CultureInfo.InvariantCulture) +
                    " private display adapters from " + source.DisplayName +
                    ". Restart the game once to load the updated local mod.";
            return "Automatic display adapter is ready for " + source.DisplayName +
                " (" + count.ToString(CultureInfo.InvariantCulture) + " private display identities).";
        }

        private static string PrivateMaterialPath(string seed)
        {
            return "/automat/pm/" + ShortHash(seed, 8) + ".mat";
        }

        private static string ShortHash(string value, int byteCount)
        {
            byte[] hash = HashBytes(value);
            StringBuilder text = new StringBuilder(byteCount * 2);
            for (int index = 0; index < byteCount; ++index)
                text.Append(hash[index].ToString("x2", CultureInfo.InvariantCulture));
            return text.ToString();
        }

        private static byte[] HashBytes(string value)
        {
            using (SHA256 sha = SHA256.Create())
                return sha.ComputeHash(Encoding.UTF8.GetBytes(value ?? ""));
        }

        private static string ReadMarkerValue(string path, string key)
        {
            try
            {
                foreach (string line in File.ReadAllLines(path))
                {
                    int equals = line.IndexOf('=');
                    if (equals > 0 && string.Equals(line.Substring(0, equals).Trim(),
                            key, StringComparison.OrdinalIgnoreCase))
                        return line.Substring(equals + 1).Trim();
                }
            }
            catch { }
            return null;
        }

        private static string ReadTextIfPresent(string path)
        {
            try { return File.Exists(path) ? File.ReadAllText(path) : ""; }
            catch { return ""; }
        }

        private static void WriteManifest(string modRoot)
        {
            string manifest = "SiiNunit\r\n{\r\n" +
                "mod_package : .prismmedia_generated {\r\n" +
                " package_version: \"4.0.0\"\r\n" +
                " display_name: \"PrismMedia Generated Display\"\r\n" +
                " author: \"PrismMedia\"\r\n" +
                " category[]: \"interior\"\r\n" +
                "}\r\n}\r\n";
            File.WriteAllText(Path.Combine(modRoot, "manifest.sii"), manifest,
                new UTF8Encoding(false));
            File.WriteAllText(Path.Combine(modRoot, "description.txt"),
                "Automatically generated private display adapters. Keep this mod above the source TJS accessory mod.",
                new UTF8Encoding(false));
        }

        private static void WriteBytes(string root, string virtualPath, byte[] bytes)
        {
            string relative = Normalize(virtualPath).TrimStart('/')
                .Replace('/', Path.DirectorySeparatorChar);
            string destination = Path.Combine(root, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(destination));
            File.WriteAllBytes(destination, bytes);
        }

        private static int ReplaceBytes(byte[] value, byte[] before, byte[] after)
        {
            if (before.Length != after.Length)
                throw new InvalidDataException("Private material path length is invalid.");
            int replacements = 0;
            for (int offset = 0; offset <= value.Length - before.Length; ++offset)
            {
                bool match = true;
                for (int index = 0; index < before.Length; ++index)
                    match &= value[offset + index] == before[index];
                if (!match) continue;
                Buffer.BlockCopy(after, 0, value, offset, after.Length);
                ++replacements;
                offset += before.Length - 1;
            }
            return replacements;
        }

        private static ZipArchiveEntry FindEntry(ZipArchive archive, string path)
        {
            string wanted = Normalize(path).TrimStart('/');
            return archive.Entries.FirstOrDefault(entry => string.Equals(
                Normalize(entry.FullName).TrimStart('/'), wanted,
                StringComparison.OrdinalIgnoreCase));
        }

        private static string ReadQuoted(string text, string key)
        {
            Match match = Regex.Match(text ?? "", @"(?mi)^\s*" + Regex.Escape(key) +
                @"\s*:\s*""([^""]*)""");
            return match.Success ? match.Groups[1].Value : null;
        }

        private static string ReplaceQuotedPath(string text, string oldPath, string newPath)
        {
            return Regex.Replace(text, "\"" + Regex.Escape(oldPath) + "\"",
                "\"" + newPath + "\"", RegexOptions.IgnoreCase);
        }

        private static string GetArgument(string[] args, string name)
        {
            for (int index = 0; index + 1 < args.Length; ++index)
                if (string.Equals(args[index], name, StringComparison.OrdinalIgnoreCase))
                    return args[index + 1];
            return null;
        }

        private static string Normalize(string value) { return (value ?? "").Replace('\\', '/'); }

        private static string SafeName(string value)
        {
            string result = new string((value ?? "display").Where(character =>
                char.IsLetterOrDigit(character) || character == '_' || character == '-').ToArray());
            return string.IsNullOrWhiteSpace(result) ? "display" : result;
        }

        private static string DecodeText(byte[] value)
        {
            using (MemoryStream stream = new MemoryStream(value))
            using (StreamReader reader = new StreamReader(stream, Encoding.UTF8, true))
                return reader.ReadToEnd();
        }

        private static string IniValue(string value)
        {
            return (value ?? "").Replace("\r", " ").Replace("\n", " ");
        }

        private static void WriteError(string path, string message)
        {
            WriteResult(path, "[Result]\r\nStatus=error\r\nMessage=" +
                IniValue(message) + "\r\nCount=0\r\n");
        }

        private static void WriteResult(string path, string text)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            string directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
            File.WriteAllText(path, text, new UTF8Encoding(false));
        }
    }
}
