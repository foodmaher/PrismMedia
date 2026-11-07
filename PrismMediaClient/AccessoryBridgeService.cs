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
        private sealed class Accessory
        {
            public string Name;
            public string DataPath;
            public string Source;
            public string Definition;
            public string InteriorModel;
            public string ExteriorModel;
            public string UiTexture;
        }

        private sealed class ArchiveView : IDisposable
        {
            private readonly Stream owner;
            private readonly ZipArchive archive;

            private ArchiveView(Stream owner, ZipArchive archive)
            {
                this.owner = owner;
                this.archive = archive;
            }

            public static ArchiveView Open(string locator)
            {
                string[] parts = locator.Split(new[] { '|' }, 2);
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
                return FindEntry(archive, path) != null;
            }

            public byte[] Read(string path)
            {
                ZipArchiveEntry entry = FindEntry(archive, path);
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
                get { return archive.Entries.Select(e => Normalize(e.FullName)); }
            }

            public void Dispose()
            {
                archive.Dispose();
                owner.Dispose();
            }
        }

        private static readonly Regex UnitRegex = new Regex(
            @"(?ms)^\s*([A-Za-z0-9_]+)\s*:\s*([^\s\{]+)\s*\{(.*?)^\s*\}",
            RegexOptions.Compiled);
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
                if (string.Equals(mode, "scan", StringComparison.OrdinalIgnoreCase))
                    Scan(args, result);
                else if (string.Equals(mode, "generate", StringComparison.OrdinalIgnoreCase))
                    Generate(args, result);
                else
                    WriteError(result, "Unknown accessory bridge mode.");
            }
            catch (Exception error)
            {
                WriteError(result, error.Message);
            }
            return true;
        }

        private static void Scan(string[] args, string resultPath)
        {
            string gameHome = GetArgument(args, "--game-home");
            string gameExe = GetArgument(args, "--game-exe");
            string save = FindNewestSave(gameHome);
            if (string.IsNullOrWhiteSpace(save))
            {
                WriteError(resultPath,
                    "No truck save was found. Leave the upgrade shop and make one quicksave.");
                return;
            }

            string text = ReadSave(save);
            List<string> dataPaths = InstalledAccessoryPaths(text);
            if (dataPaths.Count == 0)
            {
                WriteError(resultPath,
                    "The newest save has no readable installed accessory list. Set g_save_format to 2, then quicksave once.");
                return;
            }

            List<string> archives = FindModArchives(gameHome, gameExe);
            List<Accessory> found = new List<Accessory>();
            foreach (string dataPath in dataPaths.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                string entryPath = Normalize(dataPath).TrimStart('/');
                foreach (string locator in archives)
                {
                    try
                    {
                        using (ArchiveView archive = ArchiveView.Open(locator))
                        {
                            if (!archive.Exists(entryPath))
                                continue;
                            string definition = DecodeText(archive.Read(entryPath));
                            Accessory item = ParseAccessory(definition);
                            if (item == null)
                                continue;
                            item.DataPath = dataPath;
                            item.Source = locator;
                            item.Definition = entryPath;
                            found.Add(item);
                            break;
                        }
                    }
                    catch
                    {
                        // Non-ZIP SCS archives and unrelated broken mods are ignored.
                    }
                }
            }

            StringBuilder output = new StringBuilder();
            output.AppendLine("[Result]");
            output.AppendLine("Status=ok");
            output.AppendLine("Message=" + IniValue(found.Count == 0
                ? "No installed display accessory was found in ZIP-compatible local or Workshop mods."
                : "Installed display accessories found: " + found.Count.ToString(CultureInfo.InvariantCulture)));
            output.AppendLine("Save=" + IniValue(save));
            output.AppendLine("Count=" + found.Count.ToString(CultureInfo.InvariantCulture));
            for (int index = 0; index < found.Count; ++index)
            {
                Accessory item = found[index];
                output.AppendLine();
                output.AppendLine("[Accessory" + index.ToString(CultureInfo.InvariantCulture) + "]");
                output.AppendLine("Name=" + IniValue(item.Name));
                output.AppendLine("DataPath=" + IniValue(item.DataPath));
                output.AppendLine("Source=" + IniValue(item.Source));
                output.AppendLine("Definition=" + IniValue(item.Definition));
                output.AppendLine("InteriorModel=" + IniValue(item.InteriorModel));
                output.AppendLine("ExteriorModel=" + IniValue(item.ExteriorModel));
                output.AppendLine("UiTexture=" + IniValue(item.UiTexture));
            }
            WriteResult(resultPath, output.ToString());
        }

        private static void Generate(string[] args, string resultPath)
        {
            string gameHome = GetArgument(args, "--game-home");
            string source = GetArgument(args, "--source");
            string definitionPath = Normalize(GetArgument(args, "--definition")).TrimStart('/');
            string displayId = SafeName(GetArgument(args, "--display-id"));
            string prismTexture = GetArgument(args, "--media-tobj");
            if (string.IsNullOrWhiteSpace(gameHome) ||
                string.IsNullOrWhiteSpace(source) ||
                string.IsNullOrWhiteSpace(definitionPath) ||
                string.IsNullOrWhiteSpace(prismTexture))
                throw new InvalidDataException("The selected accessory is incomplete.");

            string modRoot = Path.Combine(gameHome, "mod",
                "PrismMedia_Generated_Display");
            Directory.CreateDirectory(modRoot);

            using (ArchiveView archive = ArchiveView.Open(source))
            {
                string definition = DecodeText(archive.Read(definitionPath));
                Accessory item = ParseAccessory(definition);
                if (item == null)
                    throw new InvalidDataException("The selected definition is not a display accessory.");

                Dictionary<string, string> modelMap = new Dictionary<string, string>(
                    StringComparer.OrdinalIgnoreCase);
                foreach (string model in new[] { item.InteriorModel, item.ExteriorModel })
                {
                    if (string.IsNullOrWhiteSpace(model) || modelMap.ContainsKey(model))
                        continue;
                    modelMap[model] = CopyPrivateModel(
                        archive, modRoot, model, item.UiTexture,
                        prismTexture, displayId);
                }

                foreach (KeyValuePair<string, string> pair in modelMap)
                    definition = ReplaceQuotedPath(definition, pair.Key, pair.Value);
                WriteBytes(modRoot, definitionPath,
                    new UTF8Encoding(false).GetBytes(definition));
            }

            WriteManifest(modRoot);
            WriteResult(resultPath,
                "[Result]\r\nStatus=ok\r\nMessage=" +
                IniValue("Generated local mod: " + modRoot) +
                "\r\nModPath=" + IniValue(modRoot) + "\r\n");
        }

        private static string CopyPrivateModel(ArchiveView archive,
            string modRoot, string modelPath, string uiTexture,
            string prismTexture, string displayId)
        {
            string normalized = Normalize(modelPath).TrimStart('/');
            string extension = Path.GetExtension(normalized);
            string stem = normalized.Substring(0, normalized.Length - extension.Length);
            string privateStem = "vehicle/prismmedia/" + displayId + "/" +
                SafeName(Path.GetFileName(stem));
            byte[] pmd = archive.Read(normalized);

            string pmdText = Encoding.ASCII.GetString(pmd);
            string selectedMaterial = null;
            foreach (Match match in MaterialPathRegex.Matches(pmdText))
            {
                string materialPath = Normalize(match.Value).TrimStart('/');
                if (!archive.Exists(materialPath))
                    continue;
                string material = DecodeText(archive.Read(materialPath));
                if (material.IndexOf(uiTexture,
                        StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    selectedMaterial = match.Value;
                    string privateMaterial = PrivateMaterialPath(displayId);
                    byte[] before = Encoding.ASCII.GetBytes(selectedMaterial);
                    byte[] after = Encoding.ASCII.GetBytes(privateMaterial);
                    if (before.Length != after.Length)
                        throw new InvalidDataException("Private material path length is invalid.");
                    ReplaceBytes(pmd, before, after);
                    material = ReplaceQuotedPath(material, uiTexture, prismTexture);
                    WriteBytes(modRoot, privateMaterial.TrimStart('/'),
                        new UTF8Encoding(false).GetBytes(material));
                    break;
                }
            }
            if (selectedMaterial == null)
                throw new InvalidDataException(
                    "The model material that uses the GPS picture could not be identified.");

            WriteBytes(modRoot, privateStem + ".pmd", pmd);
            foreach (string suffix in new[] { ".pmg", ".pma", ".pmc" })
            {
                string sourcePath = stem + suffix;
                if (archive.Exists(sourcePath))
                    WriteBytes(modRoot, privateStem + suffix,
                        archive.Read(sourcePath));
            }
            return "/" + privateStem + ".pmd";
        }

        private static string PrivateMaterialPath(string displayId)
        {
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hash = sha.ComputeHash(Encoding.UTF8.GetBytes(displayId));
                StringBuilder value = new StringBuilder("/automat/pm/");
                for (int index = 0; index < 8; ++index)
                    value.Append(hash[index].ToString("x2", CultureInfo.InvariantCulture));
                value.Append(".mat");
                return value.ToString();
            }
        }

        private static Accessory ParseAccessory(string definition)
        {
            string interior = ReadQuoted(definition, "interior_model");
            if (string.IsNullOrWhiteSpace(interior))
                interior = ReadQuoted(definition, "model");
            string exterior = ReadQuoted(definition, "exterior_model");
            string texture = ReadQuoted(definition, "ui_drawable_texture_path");
            if (string.IsNullOrWhiteSpace(interior) ||
                string.IsNullOrWhiteSpace(texture))
                return null;
            return new Accessory
            {
                Name = ReadQuoted(definition, "name") ?? Path.GetFileNameWithoutExtension(interior),
                InteriorModel = Normalize(interior),
                ExteriorModel = string.IsNullOrWhiteSpace(exterior)
                    ? Normalize(interior) : Normalize(exterior),
                UiTexture = Normalize(texture)
            };
        }

        private static List<string> InstalledAccessoryPaths(string text)
        {
            Dictionary<string, string> units = new Dictionary<string, string>(
                StringComparer.OrdinalIgnoreCase);
            string playerBody = null;
            foreach (Match unit in UnitRegex.Matches(text))
            {
                string type = unit.Groups[1].Value;
                string id = TrimToken(unit.Groups[2].Value);
                string body = unit.Groups[3].Value;
                units[type + "|" + id] = body;
                if (string.Equals(type, "player", StringComparison.OrdinalIgnoreCase))
                    playerBody = body;
            }
            if (playerBody == null)
                return new List<string>();
            string truck = ReadToken(playerBody, "my_truck") ??
                ReadToken(playerBody, "assigned_truck");
            string vehicleBody;
            if (truck == null || !units.TryGetValue("vehicle|" + truck, out vehicleBody))
                return new List<string>();

            HashSet<string> accessoryIds = new HashSet<string>(
                Regex.Matches(vehicleBody, @"(?m)^\s*accessories\[\d+\]\s*:\s*([^\s]+)")
                    .Cast<Match>().Select(m => TrimToken(m.Groups[1].Value)),
                StringComparer.OrdinalIgnoreCase);
            List<string> paths = new List<string>();
            foreach (KeyValuePair<string, string> unit in units)
            {
                int divider = unit.Key.IndexOf('|');
                if (divider < 0 || !unit.Key.Substring(0, divider).StartsWith(
                        "vehicle_", StringComparison.OrdinalIgnoreCase) ||
                    !accessoryIds.Contains(unit.Key.Substring(divider + 1)))
                    continue;
                string path = ReadQuoted(unit.Value, "data_path");
                if (!string.IsNullOrWhiteSpace(path))
                    paths.Add(path);
            }
            return paths;
        }

        private static string FindNewestSave(string gameHome)
        {
            if (string.IsNullOrWhiteSpace(gameHome) || !Directory.Exists(gameHome))
                return null;
            IEnumerable<string> candidates;
            try
            {
                candidates = Directory.EnumerateFiles(gameHome, "game.sii",
                    SearchOption.AllDirectories)
                    .Where(path => path.IndexOf(Path.DirectorySeparatorChar +
                        "save" + Path.DirectorySeparatorChar,
                        StringComparison.OrdinalIgnoreCase) >= 0);
                return candidates.OrderByDescending(File.GetLastWriteTimeUtc)
                    .FirstOrDefault();
            }
            catch
            {
                return null;
            }
        }

        private static string ReadSave(string path)
        {
            byte[] bytes = File.ReadAllBytes(path);
            string text = DecodeText(bytes);
            if (!text.TrimStart().StartsWith("SiiNunit",
                    StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(
                    "The newest save is encoded. Set g_save_format to 2 and quicksave once.");
            return text;
        }

        private static List<string> FindModArchives(string gameHome, string gameExe)
        {
            List<string> files = new List<string>();
            AddArchives(files, Path.Combine(gameHome ?? "", "mod"), true);
            try
            {
                DirectoryInfo directory = new FileInfo(gameExe).Directory;
                while (directory != null && !string.Equals(directory.Name,
                    "steamapps", StringComparison.OrdinalIgnoreCase))
                    directory = directory.Parent;
                if (directory != null)
                    AddArchives(files, Path.Combine(directory.FullName,
                        "workshop", "content", "227300"), true);
            }
            catch { }

            List<string> locators = new List<string>();
            foreach (string file in files.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                locators.Add(file);
                try
                {
                    using (FileStream stream = File.Open(file, FileMode.Open,
                        FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    using (ZipArchive zip = new ZipArchive(stream, ZipArchiveMode.Read))
                    {
                        foreach (ZipArchiveEntry entry in zip.Entries)
                        {
                            if (entry.FullName.EndsWith(".scs",
                                    StringComparison.OrdinalIgnoreCase) ||
                                entry.FullName.EndsWith(".zip",
                                    StringComparison.OrdinalIgnoreCase))
                                locators.Add(file + "|" + entry.FullName);
                        }
                    }
                }
                catch { }
            }
            return locators;
        }

        private static void AddArchives(List<string> files, string root, bool recursive)
        {
            if (!Directory.Exists(root))
                return;
            try
            {
                SearchOption option = recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;
                files.AddRange(Directory.EnumerateFiles(root, "*.scs", option));
                files.AddRange(Directory.EnumerateFiles(root, "*.zip", option));
            }
            catch { }
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
                "Local PrismMedia adapter. Keep this mod above the source accessory mod.",
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

        private static void ReplaceBytes(byte[] value, byte[] before, byte[] after)
        {
            for (int offset = 0; offset <= value.Length - before.Length; ++offset)
            {
                bool match = true;
                for (int index = 0; index < before.Length; ++index)
                    match &= value[offset + index] == before[index];
                if (!match)
                    continue;
                Buffer.BlockCopy(after, 0, value, offset, after.Length);
                return;
            }
            throw new InvalidDataException("The model material reference changed unexpectedly.");
        }

        private static ZipArchiveEntry FindEntry(ZipArchive archive, string path)
        {
            string wanted = Normalize(path).TrimStart('/');
            return archive.Entries.FirstOrDefault(e => string.Equals(
                Normalize(e.FullName).TrimStart('/'), wanted,
                StringComparison.OrdinalIgnoreCase));
        }

        private static string ReadQuoted(string text, string key)
        {
            Match match = Regex.Match(text, @"(?mi)^\s*" + Regex.Escape(key) +
                @"\s*:\s*""([^""]*)""");
            return match.Success ? match.Groups[1].Value : null;
        }

        private static string ReadToken(string text, string key)
        {
            Match match = Regex.Match(text, @"(?mi)^\s*" + Regex.Escape(key) +
                @"\s*:\s*([^\s]+)");
            return match.Success ? TrimToken(match.Groups[1].Value) : null;
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

        private static string TrimToken(string value)
        {
            return (value ?? "").Trim().Trim('"');
        }

        private static string Normalize(string value)
        {
            return (value ?? "").Replace('\\', '/');
        }

        private static string SafeName(string value)
        {
            string result = new string((value ?? "display").Where(
                character => char.IsLetterOrDigit(character) ||
                    character == '_' || character == '-').ToArray());
            return string.IsNullOrWhiteSpace(result) ? "display" : result;
        }

        private static string DecodeText(byte[] value)
        {
            using (MemoryStream stream = new MemoryStream(value))
            using (StreamReader reader = new StreamReader(stream, Encoding.UTF8,
                true))
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
            if (string.IsNullOrWhiteSpace(path))
                return;
            string directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(directory))
                Directory.CreateDirectory(directory);
            File.WriteAllText(path, text, new UTF8Encoding(false));
        }
    }
}
