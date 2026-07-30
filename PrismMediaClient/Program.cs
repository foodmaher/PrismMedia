using System;
using System.Globalization;
using System.Windows.Forms;

namespace PrismMediaClient
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            int parentProcessId = 0;
            bool silent = false;
            string initialUrl = null;
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
                else if (!args[index].StartsWith("--"))
                {
                    initialUrl = args[index];
                }
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(
                new MainForm(initialUrl, parentProcessId, silent));
        }
    }
}
