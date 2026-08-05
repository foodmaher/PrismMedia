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
        private const string LocalPlayerUrl =
            "https://prism.local/player.html";
        private const int GwlExStyle = -20;
        private const int WsExNoActivate = 0x08000000;
        private const uint KeyEventKeyUp = 0x0002;
        private const uint ThreadSetInformation = 0x0020;

        [DllImport("user32.dll")]
        private static extern int GetWindowLong(IntPtr window, int index);

        [DllImport("user32.dll")]
        private static extern int SetWindowLong(
            IntPtr window, int index, int value);

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr window);

        [DllImport("user32.dll")]
        private static extern void keybd_event(
            byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenThread(
            uint desiredAccess, bool inheritHandle, uint threadId);

        [DllImport("kernel32.dll")]
        private static extern uint SetThreadIdealProcessor(
            IntPtr thread, uint idealProcessor);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);

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
        private bool fullSpotifyWeb;
        private bool userWantsPlayback = true;
        private bool vehiclePowered = true;

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
            ShowInTaskbar = !silentStart;
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
                string profileFolder = Path.Combine(
                    Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData),
                    "PrismTextureStreamerFB", "WebView2Profile");
                Directory.CreateDirectory(profileFolder);
                CoreWebView2Environment environment =
                    await CoreWebView2Environment.CreateAsync(
                        null, profileFolder);
                await webView.EnsureCoreWebView2Async(environment);
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
                webView.CoreWebView2.NavigationCompleted += async (_, args) =>
                {
                    ClientDiagnosticLog.Write(
                        args.IsSuccess ? "navigation" : "error",
                        "Navigation completed: success=" +
                        args.IsSuccess + ", status=" +
                        args.WebErrorStatus + ", source=" +
                        webView.Source);
                    ready = true;
                    while (pendingCommands.Count > 0)
                    {
                        await ApplyCommandAsync(pendingCommands.Dequeue());
                    }
                };
                webView.CoreWebView2.ProcessFailed += (_, args) =>
                {
                    ClientDiagnosticLog.Write(
                        "error", "WebView2 process failure: " +
                        args.ProcessFailedKind + ".");
                };
                webView.CoreWebView2.NewWindowRequested += (_, args) =>
                {
                    if (!fullSpotifyWeb || string.IsNullOrWhiteSpace(args.Uri))
                        return;
                    args.Handled = true;
                    ClientDiagnosticLog.Write(
                        "spotify", "Opening Spotify popup/login navigation in " +
                        "the existing helper: " + args.Uri);
                    webView.CoreWebView2.Navigate(args.Uri);
                };
                webView.CoreWebView2.Navigate(LocalPlayerUrl);
                ClientDiagnosticLog.Write(
                    "webview", "WebView2 initialization completed with a " +
                    "persistent profile at " + profileFolder + ".");
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

        private static string NormalizeSpotifyUrl(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
                return "https://open.spotify.com/";

            value = value.Trim();
            if (value.StartsWith("spotify:", StringComparison.OrdinalIgnoreCase))
            {
                string[] parts = value.Split(':');
                if (parts.Length == 3)
                    return "https://open.spotify.com/" + parts[1] + "/" +
                        parts[2];
            }

            if (Uri.TryCreate(value, UriKind.Absolute, out Uri uri))
            {
                string host = uri.Host.StartsWith(
                    "www.", StringComparison.OrdinalIgnoreCase)
                    ? uri.Host.Substring(4) : uri.Host;
                if (!string.Equals(
                    host, "open.spotify.com",
                    StringComparison.OrdinalIgnoreCase))
                    return "https://open.spotify.com/";
                string path = uri.AbsolutePath;
                if (path.StartsWith("/embed/", StringComparison.OrdinalIgnoreCase))
                    path = path.Substring(6);
                return "https://open.spotify.com" + path + uri.Query;
            }
            return "https://open.spotify.com/";
        }

        private async Task NavigateToLocalPlayerAsync(string queuedCommand)
        {
            fullSpotifyWeb = false;
            ready = false;
            if (!string.IsNullOrEmpty(queuedCommand))
                pendingCommands.Enqueue(queuedCommand);
            ClientDiagnosticLog.Write(
                "spotify", "Returning to the optimized local player.");
            webView.CoreWebView2.Navigate(LocalPlayerUrl);
            await Task.CompletedTask;
        }

        private async Task NavigateToFullSpotifyAsync(string value)
        {
            fullSpotifyWeb = true;
            userWantsPlayback = true;
            ready = false;
            string target = NormalizeSpotifyUrl(value);
            ClientDiagnosticLog.Write(
                "spotify", "Navigating to Full Spotify Web Player: " +
                target);
            webView.CoreWebView2.Navigate(target);
            await Task.CompletedTask;
        }

        private void SetInteractiveWindow(bool interactive)
        {
            int style = GetWindowLong(Handle, GwlExStyle);
            if (interactive)
            {
                SetWindowLong(Handle, GwlExStyle, style & ~WsExNoActivate);
                FormBorderStyle = FormBorderStyle.Sizable;
                ShowInTaskbar = true;
                WindowState = FormWindowState.Normal;
                BringToFront();
                Activate();
                SetForegroundWindow(Handle);
                ClientDiagnosticLog.Write(
                    "spotify", "Interactive Spotify login window opened.");
            }
            else
            {
                FormBorderStyle = FormBorderStyle.None;
                ShowInTaskbar = false;
                SetWindowLong(Handle, GwlExStyle, style | WsExNoActivate);
                ClientDiagnosticLog.Write(
                    "spotify", "Spotify helper returned to silent mode.");
            }
        }

        private static void SendMediaKey(byte virtualKey)
        {
            keybd_event(virtualKey, 0, 0, UIntPtr.Zero);
            keybd_event(virtualKey, 0, KeyEventKeyUp, UIntPtr.Zero);
        }

        private static int ApplySoftCpuHintsToProcess(
            uint processId, IReadOnlyList<uint> processors)
        {
            if (processId == 0 || processId > Int32.MaxValue ||
                processors == null || processors.Count == 0)
                return 0;
            int applied = 0;
            try
            {
                using (Process process = Process.GetProcessById(
                    unchecked((int)processId)))
                {
                    int index = 0;
                    foreach (ProcessThread thread in process.Threads)
                    {
                        IntPtr handle = OpenThread(
                            ThreadSetInformation, false,
                            unchecked((uint)thread.Id));
                        if (handle == IntPtr.Zero)
                            continue;
                        try
                        {
                            uint result = SetThreadIdealProcessor(
                                handle, processors[index % processors.Count]);
                            if (result != UInt32.MaxValue)
                                ++applied;
                            ++index;
                        }
                        finally
                        {
                            CloseHandle(handle);
                        }
                    }
                }
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "scheduler", "Could not apply all soft CPU hints to pid " +
                    processId + ": " + error.Message);
            }
            return applied;
        }

        private void ApplySoftCpuHints(string value)
        {
            string[] parts = value.Split(',');
            var processors = new List<uint>();
            foreach (string part in parts)
            {
                if (uint.TryParse(
                    part, NumberStyles.Integer,
                    CultureInfo.InvariantCulture, out uint processor) &&
                    processor < 64)
                    processors.Add(processor);
            }
            if (processors.Count == 0)
                return;

            int helperThreads = ApplySoftCpuHintsToProcess(
                unchecked((uint)Process.GetCurrentProcess().Id), processors);
            int browserThreads = webView.CoreWebView2 == null
                ? 0
                : ApplySoftCpuHintsToProcess(
                    webView.CoreWebView2.BrowserProcessId, processors);
            ClientDiagnosticLog.Write(
                "scheduler", "Applied soft CPU hints [" +
                string.Join(",", processors) + "] to " + helperThreads +
                " helper and " + browserThreads + " browser threads.");
        }

        private async Task<bool> TrySpotifyDomCommandAsync(string command)
        {
            string script;
            switch (command)
            {
                case "playpause":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-playpause\"]'); if(!b)return false; b.click(); return true; })()";
                    break;
                case "play":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-playpause\"]'); if(!b)return false; const a=(b.getAttribute('aria-label')||'').toLowerCase(); if(a.includes('play'))b.click(); return true; })()";
                    break;
                case "pause":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-playpause\"]'); if(!b)return false; const a=(b.getAttribute('aria-label')||'').toLowerCase(); if(a.includes('pause'))b.click(); return true; })()";
                    break;
                case "next":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-skip-forward\"]'); if(!b)return false; b.click(); return true; })()";
                    break;
                case "previous":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-skip-back\"]'); if(!b)return false; b.click(); return true; })()";
                    break;
                case "mute":
                    script = "(() => { const b=document.querySelector('[data-testid=\"volume-bar-toggle-mute-button\"]'); if(!b)return false; b.click(); return true; })()";
                    break;
                case "volumeup":
                case "volumedown":
                {
                    int direction = command == "volumeup" ? 1 : -1;
                    script = "(() => { const s=document.querySelector('[data-testid=\"volume-bar\"] input, input[type=\"range\"][aria-label*=\"volume\" i]'); if(!s)return false; const lo=Number(s.min||0), hi=Number(s.max||1), step=(hi-lo)*0.05; s.value=Math.max(lo,Math.min(hi,Number(s.value)+(" + direction + ")*step)); s.dispatchEvent(new Event('input',{bubbles:true})); s.dispatchEvent(new Event('change',{bubbles:true})); return true; })()";
                    break;
                }
                default:
                    return false;
            }

            try
            {
                string result = await webView.CoreWebView2.ExecuteScriptAsync(script);
                bool handled = string.Equals(
                    result, "true", StringComparison.OrdinalIgnoreCase);
                ClientDiagnosticLog.Write(
                    "spotify", "DOM command " + command +
                    (handled ? " succeeded." : " did not find its control."));
                return handled;
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "Spotify DOM command " + command +
                    " failed: " + error.Message);
                return false;
            }
        }

        private async Task ExecuteFullSpotifyCommandAsync(string command)
        {
            int separator = command.IndexOf('|');
            string name = separator < 0 ? command : command.Substring(0, separator);
            string argument = separator < 0 ? "" : command.Substring(separator + 1);

            if (name == "vehiclepower")
            {
                vehiclePowered = argument == "1";
                name = vehiclePowered && userWantsPlayback ? "play" : "pause";
            }
            else if (name == "playpause")
            {
                userWantsPlayback = !userWantsPlayback;
                name = userWantsPlayback && vehiclePowered ? "play" : "pause";
            }
            else if (name == "play")
            {
                userWantsPlayback = true;
                if (!vehiclePowered)
                    name = "pause";
            }
            else if (name == "pause")
            {
                userWantsPlayback = false;
            }
            else if (name == "brightness")
            {
                if (!double.TryParse(
                    argument, NumberStyles.Float,
                    CultureInfo.InvariantCulture, out double brightness))
                    brightness = 1.0;
                brightness = Math.Max(0.1, Math.Min(2.0, brightness));
                double darkness = brightness < 1.0 ? 1.0 - brightness : 0.0;
                string script = "(() => { let o=document.getElementById('prism-brightness-overlay'); if(!o){o=document.createElement('div');o.id='prism-brightness-overlay';o.style.cssText='position:fixed;inset:0;z-index:2147483647;background:#000;pointer-events:none';document.documentElement.appendChild(o);} o.style.opacity='" + darkness.ToString("0.0000", CultureInfo.InvariantCulture) + "'; document.documentElement.style.filter='" + (brightness > 1.0 ? "brightness(" + brightness.ToString("0.0000", CultureInfo.InvariantCulture) + ")" : "none") + "'; return true; })()";
                await webView.CoreWebView2.ExecuteScriptAsync(script);
                return;
            }

            bool handled = await TrySpotifyDomCommandAsync(name);
            if (handled)
                return;

            byte key;
            switch (name)
            {
                case "play":
                case "pause":
                case "playpause": key = 0xB3; break;
                case "next": key = 0xB0; break;
                case "previous": key = 0xB1; break;
                case "mute": key = 0xAD; break;
                case "volumeup": key = 0xAF; break;
                case "volumedown": key = 0xAE; break;
                default: return;
            }
            SendMediaKey(key);
            ClientDiagnosticLog.Write(
                "spotify", "Used Windows media-key fallback for " + name + ".");
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
            if (command.StartsWith("cpuhints|", StringComparison.Ordinal))
            {
                ApplySoftCpuHints(command.Substring(9));
                return;
            }
            if (string.Equals(command, "spotifylogin", StringComparison.Ordinal))
            {
                SetInteractiveWindow(true);
                return;
            }
            if (string.Equals(command, "spotifyhide", StringComparison.Ordinal))
            {
                SetInteractiveWindow(false);
                return;
            }
            if (string.Equals(command, "clearspotify", StringComparison.Ordinal))
            {
                await webView.CoreWebView2.Profile.ClearBrowsingDataAsync();
                ClientDiagnosticLog.Write(
                    "spotify", "Persistent Spotify/WebView session cleared.");
                await NavigateToLocalPlayerAsync(null);
                return;
            }
            if (command.StartsWith("loadspotifyweb|", StringComparison.Ordinal))
            {
                await NavigateToFullSpotifyAsync(command.Substring(15));
                return;
            }
            if (command.StartsWith("load|", StringComparison.Ordinal) &&
                fullSpotifyWeb)
            {
                await NavigateToLocalPlayerAsync(command);
                return;
            }
            if (fullSpotifyWeb)
            {
                await ExecuteFullSpotifyCommandAsync(command);
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
