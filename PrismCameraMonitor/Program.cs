using System;
using System.Threading;
using System.Windows.Forms;

namespace PrismCameraMonitor
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            using (var singleInstance = new Mutex(
                true, "Local\\PrismCameraMonitorViewerV1", out bool owner))
            {
                if (!owner)
                    return;
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MonitorForm());
            }
        }
    }
}
