using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;
using System;
using System.Collections.Generic;
using System.IO;
using System.Globalization;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PrismMediaClient
{
    internal sealed class MainForm : Form
    {
        private const int WmCopyData = 0x004A;
        private const int WmPrismEnvironment = 0x8351;
        private const ulong PrismCopyDataId = 0x50524953;

        [StructLayout(LayoutKind.Sequential)]
        private struct CopyDataStruct
        {
            public UIntPtr DataId;
            public int ByteCount;
            public IntPtr Data;
        }

        private readonly WebView2 webView = new WebView2();
        private readonly Queue<string> pendingCommands =
            new Queue<string>();
        private readonly AdaptiveAudioController adaptiveAudio =
            new AdaptiveAudioController();
        private MediaLowPassController lowPass;
        private readonly Timer parentMonitor = new Timer();
        private int parentProcessId;
        private readonly bool silentStart;
        private bool ready;
        private bool initializing;

        internal MainForm(
            string initialUrl,
            int parentProcessId,
            bool silentStart)
        {
            this.parentProcessId = parentProcessId;
            this.silentStart = silentStart;
            Text = "Prism Media Client";
            StartPosition = FormStartPosition.CenterScreen;
            Width = 1280;
            Height = 720;
            MinimumSize = new System.Drawing.Size(426, 240);
            BackColor = System.Drawing.Color.Black;
            FormBorderStyle = FormBorderStyle.None;
            KeyPreview = true;
            KeyDown += (_, key) =>
            {
                if (key.KeyCode == Keys.F11)
                    FormBorderStyle = FormBorderStyle == FormBorderStyle.None
                        ? FormBorderStyle.Sizable
                        : FormBorderStyle.None;
            };
            webView.Dock = DockStyle.Fill;
            Controls.Add(webView);
            if (!string.IsNullOrWhiteSpace(initialUrl))
                pendingCommands.Enqueue("load|" + initialUrl);
            Shown += async (_, __) =>
            {
                await InitializePlayerAsync();
            };
            parentMonitor.Interval = 1000;
            parentMonitor.Tick += (_, __) =>
            {
                if (this.parentProcessId <= 0)
                    return;
                try
                {
                    using (Process parent =
                        Process.GetProcessById(this.parentProcessId))
                    {
                        if (parent.HasExited)
                            Close();
                    }
                }
                catch (ArgumentException)
                {
                    Close();
                }
            };
            parentMonitor.Start();
        }

        protected override bool ShowWithoutActivation => silentStart;

        protected override CreateParams CreateParams
        {
            get
            {
                const int WsExNoActivate = 0x08000000;
                CreateParams parameters = base.CreateParams;
                if (silentStart)
                    parameters.ExStyle |= WsExNoActivate;
                return parameters;
            }
        }

        private async Task InitializePlayerAsync()
        {
            if (ready || initializing)
                return;
            initializing = true;
            try
            {
                ClientDiagnosticLog.Write(
                    "webview", "WebView2 initialization started.");
                await webView.EnsureCoreWebView2Async();
                webView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
                webView.CoreWebView2.Settings.AreDevToolsEnabled = false;
                webView.CoreWebView2.Settings.IsStatusBarEnabled = false;
                webView.CoreWebView2.Settings.IsZoomControlEnabled = false;
                adaptiveAudio.SetBrowserProcessId(
                    webView.CoreWebView2.BrowserProcessId);
                webView.CoreWebView2.WebMessageReceived += (_, eventArgs) =>
                {
                    try
                    {
                        string value = eventArgs.TryGetWebMessageAsString();
                        if (!value.StartsWith(
                            "log|", StringComparison.Ordinal))
                            return;
                        string[] parts = value.Split(
                            new[] { '|' }, 3);
                        if (parts.Length == 3)
                            ClientDiagnosticLog.Write(parts[1], parts[2]);
                    }
                    catch
                    {
                        // A malformed page message is ignored.
                    }
                };

                string contentFolder = Path.Combine(
                    AppDomain.CurrentDomain.BaseDirectory, "www");
                lowPass = new MediaLowPassController(
                    webView.CoreWebView2);
                await lowPass.InstallAsync(File.ReadAllText(
                    Path.Combine(contentFolder, "adaptive-audio.js")));
                webView.CoreWebView2.SetVirtualHostNameToFolderMapping(
                    "prism.local", contentFolder,
                    CoreWebView2HostResourceAccessKind.Allow);
                webView.CoreWebView2.NavigationCompleted += async (_, __) =>
                {
                    ready = true;
                    while (pendingCommands.Count > 0)
                    {
                        await ApplyCommandAsync(pendingCommands.Dequeue());
                    }
                };
                webView.CoreWebView2.Navigate("https://prism.local/player.html");
                ClientDiagnosticLog.Write(
                    "webview", "WebView2 initialization completed.");
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "WebView2 initialization failed: " +
                    error.Message);
                Text = "Prism Media Client - WebView2 error";
                MessageBox.Show(
                    "The Prism Media Client could not start WebView2.\n\n" +
                    error.Message +
                    "\n\nInstall the Microsoft Edge WebView2 Runtime and try again.",
                    "Prism Media Client", MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
            }
            finally
            {
                initializing = false;
            }
        }

        protected override void WndProc(ref Message message)
        {
            if (message.Msg == WmPrismEnvironment)
            {
                long scaledGain = message.WParam.ToInt64();
                adaptiveAudio.SetDucking(
                    Math.Max(0.0f, Math.Min(1.0f, scaledGain / 10000.0f)));
                message.Result = new IntPtr(1);
                return;
            }
            if (message.Msg == WmCopyData)
            {
                var data = Marshal.PtrToStructure<CopyDataStruct>(message.LParam);
                if (data.DataId.ToUInt64() == PrismCopyDataId &&
                    data.Data != IntPtr.Zero && data.ByteCount > 0)
                {
                    byte[] bytes = new byte[data.ByteCount];
                    Marshal.Copy(data.Data, bytes, 0, bytes.Length);
                    string command = Encoding.UTF8.GetString(bytes).TrimEnd('\0');
                    BeginInvoke(new Action(async () =>
                    {
                        bool immediate =
                            command.StartsWith(
                                "parent|", StringComparison.Ordinal);
                        if (immediate || ready)
                            await ApplyCommandAsync(command);
                        else if (string.Equals(
                            command, "initialize",
                            StringComparison.Ordinal))
                            await InitializePlayerAsync();
                        else
                        {
                            pendingCommands.Enqueue(command);
                            await InitializePlayerAsync();
                        }
                    }));
                    message.Result = new IntPtr(1);
                    return;
                }
            }
            base.WndProc(ref message);
        }

        private async Task ExecuteCommandAsync(string command)
        {
            if (webView.CoreWebView2 == null)
                return;
            string escaped = System.Web.HttpUtility.JavaScriptStringEncode(
                command, true);
            await webView.CoreWebView2.ExecuteScriptAsync(
                "window.prismCommand(" + escaped + ");");
        }

        private async Task ApplyCommandAsync(string command)
        {
            if (command.StartsWith("parent|", StringComparison.Ordinal))
            {
                if (int.TryParse(
                    command.Substring(7),
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out int newParentProcessId))
                {
                    parentProcessId = newParentProcessId;
                }
                return;
            }
            if (command.StartsWith("spatial|", StringComparison.Ordinal))
            {
                string[] parts = command.Split('|');
                if (parts.Length >= 4 &&
                    int.TryParse(parts[1], out int spatialEnabled) &&
                    float.TryParse(
                        parts[2], NumberStyles.Float,
                        CultureInfo.InvariantCulture, out float gain) &&
                    float.TryParse(
                        parts[3], NumberStyles.Float,
                        CultureInfo.InvariantCulture, out float pan))
                {
                    float cutoffHz = 20000.0f;
                    if (parts.Length >= 5)
                        float.TryParse(
                            parts[4], NumberStyles.Float,
                            CultureInfo.InvariantCulture,
                            out cutoffHz);
                    adaptiveAudio.SetDesired(
                        spatialEnabled != 0, gain, pan);
                    lowPass?.SetDesired(
                        spatialEnabled != 0,
                        cutoffHz <= 0.0f ? 20000.0f : cutoffHz);
                }
                return;
            }
            if (command.StartsWith("resize|", StringComparison.Ordinal))
            {
                string[] dimensions = command.Substring(7).Split('x');
                if (dimensions.Length == 2 &&
                    int.TryParse(dimensions[0], out int width) &&
                    int.TryParse(dimensions[1], out int height))
                {
                    ClientSize = new System.Drawing.Size(
                        Math.Max(426, Math.Min(3840, width)),
                        Math.Max(240, Math.Min(2160, height)));
                }
                return;
            }
            await ExecuteCommandAsync(command);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            parentMonitor.Stop();
            parentMonitor.Dispose();
            lowPass?.Dispose();
            adaptiveAudio.Dispose();
            base.OnFormClosed(e);
        }
    }
}
