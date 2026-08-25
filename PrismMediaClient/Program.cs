using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows.Forms;

namespace PrismMediaClient
{
    internal static class Program
    {
        private static string SafeFolderName(string value)
        {
            string result = "";
            foreach (char character in value ?? "")
            {
                if (char.IsLetterOrDigit(character) || character == '-' ||
                    character == '_')
                    result += character;
                else if (character == ' ' && result.Length > 0 &&
                    result[result.Length - 1] != '_')
                    result += '_';
            }
            return string.IsNullOrWhiteSpace(result) ? "display" : result;
        }

        private static string PrepareClientDirectory(
            string profileName, string legacyProfileSuffix)
        {
            string root = Path.Combine(
                Environment.GetFolderPath(
                    Environment.SpecialFolder.LocalApplicationData),
                "PrismMedia", "Clients");
            Directory.CreateDirectory(root);
            string destination = Path.Combine(
                root, SafeFolderName(profileName));

            // Retain existing WebView cookies/login data while replacing the
            // opaque display-ID folder with the game texture's visible name.
            string legacyRoot = Path.Combine(
                Environment.GetFolderPath(
                    Environment.SpecialFolder.LocalApplicationData),
                "PrismTextureStreamerFB");
            string legacyProfile = Path.Combine(
                legacyRoot,
                legacyProfileSuffix == "-legacy"
                    ? "WebView2Profile"
                    : "WebView2Profile" + legacyProfileSuffix);
            if (!Directory.Exists(destination) &&
                Directory.Exists(legacyProfile))
            {
                try
                {
                    Directory.Move(legacyProfile, destination);
                }
                catch
                {
                    // If antivirus or WebView keeps the old directory busy,
                    // start the named folder normally rather than failing.
                    Directory.CreateDirectory(destination);
                }
            }
            else
            {
                Directory.CreateDirectory(destination);
            }
            return destination;
        }

        [STAThread]
        private static void Main(string[] args)
        {
            try
            {
                Process.GetCurrentProcess().PriorityClass =
                    ProcessPriorityClass.BelowNormal;
            }
            catch
            {
                // The plugin launcher already requests this priority. Continue
                // normally if a restricted Windows policy rejects the hint.
            }

            int parentProcessId = 0;
            bool silent = false;
            string initialUrl = null;
            string windowTitle = "Prism Media Client";
            string profileSuffix = "";
            string profileName = "";
            for (int index = 0; index < args.Length; ++index)
            {
                if (string.Equals(
                    args[index], "--silent",
                    StringComparison.OrdinalIgnoreCase))
                {
                    silent = true;
                }
                else if (string.Equals(
                    args[index], "--parent-pid",
                    StringComparison.OrdinalIgnoreCase) &&
                    index + 1 < args.Length)
                {
                    int.TryParse(
                        args[++index], NumberStyles.Integer,
                        CultureInfo.InvariantCulture, out parentProcessId);
                }
                else if (string.Equals(
                    args[index], "--window-title",
                    StringComparison.OrdinalIgnoreCase) &&
                    index + 1 < args.Length)
                {
                    windowTitle = args[++index];
                }
                else if (string.Equals(
                    args[index], "--profile-suffix",
                    StringComparison.OrdinalIgnoreCase) &&
                    index + 1 < args.Length)
                {
                    string requested = args[++index];
                    foreach (char character in requested)
                    {
                        if (char.IsLetterOrDigit(character) ||
                            character == '-' || character == '_')
                            profileSuffix += character;
                    }
                }
                else if (string.Equals(
                    args[index], "--profile-name",
                    StringComparison.OrdinalIgnoreCase) &&
                    index + 1 < args.Length)
                {
                    profileName = SafeFolderName(args[++index]);
                }
                else if (!args[index].StartsWith("--"))
                {
                    initialUrl = args[index];
                }
            }
            if (string.IsNullOrWhiteSpace(profileName))
                profileName = SafeFolderName(profileSuffix.TrimStart('-'));
            string clientDirectory = PrepareClientDirectory(
                profileName, profileSuffix);
            ClientDiagnosticLog.Start(clientDirectory);
            try
            {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(
                    new MainForm(
                        initialUrl, parentProcessId, silent,
                        windowTitle, profileSuffix, clientDirectory));
            }
            finally
            {
                ClientDiagnosticLog.Stop();
            }
        }
    }
}
