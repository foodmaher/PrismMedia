using System;
using System.Diagnostics;
using System.Globalization;
using System.Windows.Forms;

namespace PrismMediaClient
{
    internal static class Program
    {
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
                else if (!args[index].StartsWith("--"))
                {
                    initialUrl = args[index];
                }
            }
            ClientDiagnosticLog.Start(parentProcessId);
            try
            {
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(
                    new MainForm(
                        initialUrl, parentProcessId, silent,
                        windowTitle, profileSuffix));
            }
            finally
            {
                ClientDiagnosticLog.Stop();
            }
        }
    }
}
