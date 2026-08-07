using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;
using System;
using System.Collections.Generic;
using System.IO;
using System.Globalization;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
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
        private const string TrafficLocalHost = "traffic.prism.local";
        private const int GwlExStyle = -20;
        private const int WsExNoActivate = 0x08000000;
        private const uint KeyEventKeyUp = 0x0002;
        private const uint ThreadSetInformation = 0x0020;
        private const int SpotifySessionFormatVersion = 1;

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
        private readonly string profileSuffix;
        private bool ready;
        private bool initializing;
        private bool coreInitialized;
        private bool spotifySessionRestored;
        private bool spotifySessionSaveActive;
        private bool fullSpotifyWeb;
        private bool userWantsPlayback = true;
        private bool vehiclePowered = true;
        private double desiredBrightness = 1.0;
        private double lastLoggedBrightness = -1.0;
        private DateTime lastBrightnessLogUtc = DateTime.MinValue;

        internal MainForm(
            string initialUrl,
            int parentProcessId,
            bool silentStart,
            string windowTitle,
            string profileSuffix)
        {
            this.parentProcessId = parentProcessId;
            this.silentStart = silentStart;
            this.profileSuffix = profileSuffix ?? "";
            Text = string.IsNullOrWhiteSpace(windowTitle)
                ? "Prism Media Client" : windowTitle;
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
            if (coreInitialized || webView.CoreWebView2 != null || initializing)
                return;
            initializing = true;
            try
            {
                ClientDiagnosticLog.Write(
                    "webview", "WebView2 initialization started.");
                string profileFolder = Path.Combine(
                    Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData),
                    "PrismTextureStreamerFB",
                    "WebView2Profile" + this.profileSuffix);
                Directory.CreateDirectory(profileFolder);
                var environmentOptions = new CoreWebView2EnvironmentOptions
                {
                    AdditionalBrowserArguments =
                        "--autoplay-policy=no-user-gesture-required " +
                        "--disable-backgrounding-occluded-windows " +
                        "--disable-background-timer-throttling " +
                        "--disable-renderer-backgrounding " +
                        "--disable-features=CalculateNativeWinOcclusion"
                };
                CoreWebView2Environment environment =
                    await CoreWebView2Environment.CreateAsync(
                        null, profileFolder, environmentOptions);
                await webView.EnsureCoreWebView2Async(environment);
                coreInitialized = true;
                webView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
                webView.CoreWebView2.Settings.AreDevToolsEnabled = false;
                webView.CoreWebView2.Settings.IsStatusBarEnabled = false;
                webView.CoreWebView2.Settings.IsZoomControlEnabled = false;
                adaptiveAudio.SetBrowserProcessId(
                    webView.CoreWebView2.BrowserProcessId);
                await RestoreSpotifySessionAsync();
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
                    Uri source = webView.Source;
                    ClientDiagnosticLog.Write(
                        args.IsSuccess ? "navigation" : "error",
                        "Navigation completed: success=" +
                        args.IsSuccess + ", status=" +
                        args.WebErrorStatus + ", source=" +
                        SafeUriForLog(source));
                    adaptiveAudio.RequestSessionRefresh(
                        fullSpotifyWeb
                            ? "Spotify top-level navigation"
                            : "media navigation");
                    ready = true;
                    while (pendingCommands.Count > 0)
                    {
                        await ApplyCommandAsync(pendingCommands.Dequeue());
                    }
                    if (args.IsSuccess)
                        await ApplyBrightnessAsync(
                            "top-level navigation completed");
                    if (args.IsSuccess && fullSpotifyWeb &&
                        IsSpotifyUri(source))
                    {
                        await Task.Delay(750);
                        await SaveSpotifySessionAsync(
                            "successful Spotify navigation");
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
                        "the existing helper: " +
                        SafeUriForLog(args.Uri));
                    webView.CoreWebView2.Navigate(args.Uri);
                };
                webView.CoreWebView2.Navigate(LocalPlayerUrl);
                ClientDiagnosticLog.Write(
                    "webview", "WebView2 initialization completed with a " +
                    "persistent profile at " + profileFolder +
                    "; occluded-window rendering is enabled.");
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

        private async Task ApplyBrightnessAsync(string reason)
        {
            if (webView.CoreWebView2 == null)
                return;

            string value = desiredBrightness.ToString(
                "0.0000", CultureInfo.InvariantCulture);
            try
            {
                if (!fullSpotifyWeb)
                {
                    await ExecuteCommandAsync("brightness|" + value);
                }
                else
                {
                    double darkness = desiredBrightness < 1.0
                        ? 1.0 - desiredBrightness : 0.0;
                    string script =
                        "(() => { let o=document.getElementById('prism-brightness-overlay');" +
                        "if(!o){o=document.createElement('div');" +
                        "o.id='prism-brightness-overlay';" +
                        "o.style.cssText='position:fixed;inset:0;" +
                        "z-index:2147483647;background:#000;pointer-events:none';" +
                        "document.documentElement.appendChild(o);}" +
                        "o.style.opacity='" + darkness.ToString(
                            "0.0000", CultureInfo.InvariantCulture) + "';" +
                        "document.documentElement.style.filter='" +
                        (desiredBrightness > 1.0
                            ? "brightness(" + value + ")" : "none") + "';" +
                        "return true; })()";
                    await webView.CoreWebView2.ExecuteScriptAsync(script);
                }
                DateTime now = DateTime.UtcNow;
                bool navigationReapply = !string.Equals(
                    reason, "plugin brightness command",
                    StringComparison.Ordinal);
                if (navigationReapply ||
                    Math.Abs(desiredBrightness - lastLoggedBrightness) >= 0.05 ||
                    (now - lastBrightnessLogUtc).TotalSeconds >= 5.0)
                {
                    ClientDiagnosticLog.Write(
                        "render", "Brightness " + value +
                        " reapplied after " + reason + ".");
                    lastLoggedBrightness = desiredBrightness;
                    lastBrightnessLogUtc = now;
                }
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "Brightness reapply failed after " + reason +
                    ": " + error.Message);
            }
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

        private static bool IsSpotifyUri(Uri uri)
        {
            if (uri == null || !uri.IsAbsoluteUri)
                return false;
            string host = uri.Host.ToLowerInvariant();
            return host == "spotify.com" ||
                host.EndsWith(".spotify.com", StringComparison.Ordinal);
        }

        private static bool IsSpotifyValue(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
                return false;
            value = value.Trim();
            if (value.StartsWith(
                "spotify:", StringComparison.OrdinalIgnoreCase))
                return true;
            return Uri.TryCreate(value, UriKind.Absolute, out Uri uri) &&
                IsSpotifyUri(uri);
        }

        private static string SafeUriForLog(Uri uri)
        {
            if (uri == null)
                return "<none>";
            if (!uri.IsAbsoluteUri)
                return uri.ToString();
            return uri.Scheme + "://" + uri.Host + uri.AbsolutePath;
        }

        private static string SafeUriForLog(string value)
        {
            return Uri.TryCreate(value, UriKind.Absolute, out Uri uri)
                ? SafeUriForLog(uri)
                : "<invalid URI>";
        }

        private static string SpotifySessionPath
        {
            get
            {
                string folder = Path.Combine(
                    Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData),
                    "PrismTextureStreamerFB");
                Directory.CreateDirectory(folder);
                return Path.Combine(folder, "SpotifySession.dat");
            }
        }

        private async Task<List<CoreWebView2Cookie>> GetSpotifyCookiesAsync()
        {
            var unique = new Dictionary<string, CoreWebView2Cookie>(
                StringComparer.OrdinalIgnoreCase);
            string[] origins = {
                "https://open.spotify.com/",
                "https://accounts.spotify.com/",
                "https://challenge.spotify.com/",
                "https://spotify.com/"
            };
            foreach (string origin in origins)
            {
                IReadOnlyList<CoreWebView2Cookie> cookies =
                    await webView.CoreWebView2.CookieManager
                        .GetCookiesAsync(origin);
                foreach (CoreWebView2Cookie cookie in cookies)
                {
                    string key = cookie.Domain + "\n" + cookie.Path +
                        "\n" + cookie.Name;
                    unique[key] = cookie;
                }
            }
            return new List<CoreWebView2Cookie>(unique.Values);
        }

        private async Task RestoreSpotifySessionAsync()
        {
            if (spotifySessionRestored || webView.CoreWebView2 == null)
                return;
            spotifySessionRestored = true;
            string path = SpotifySessionPath;
            if (!File.Exists(path))
            {
                ClientDiagnosticLog.Write(
                    "spotify", "No encrypted Spotify session checkpoint exists yet.");
                return;
            }

            try
            {
                byte[] encrypted = File.ReadAllBytes(path);
                byte[] payload = ProtectedData.Unprotect(
                    encrypted, null, DataProtectionScope.CurrentUser);
                int restored = 0;
                using (var stream = new MemoryStream(payload, false))
                using (var reader = new BinaryReader(stream, Encoding.UTF8))
                {
                    int version = reader.ReadInt32();
                    if (version != SpotifySessionFormatVersion)
                        throw new InvalidDataException(
                            "Unsupported Spotify session checkpoint version.");
                    int count = reader.ReadInt32();
                    if (count < 0 || count > 2048)
                        throw new InvalidDataException(
                            "Invalid Spotify session cookie count.");
                    for (int index = 0; index < count; ++index)
                    {
                        string name = reader.ReadString();
                        string value = reader.ReadString();
                        string domain = reader.ReadString();
                        string cookiePath = reader.ReadString();
                        long expiresBinary = reader.ReadInt64();
                        bool isHttpOnly = reader.ReadBoolean();
                        bool isSecure = reader.ReadBoolean();
                        var sameSite = (CoreWebView2CookieSameSiteKind)
                            reader.ReadInt32();
                        DateTime expires = expiresBinary == 0
                            ? DateTime.MinValue
                            : DateTime.FromBinary(expiresBinary).ToUniversalTime();
                        if (expiresBinary != 0 && expires <= DateTime.UtcNow)
                            continue;

                        CoreWebView2Cookie cookie = webView.CoreWebView2
                            .CookieManager.CreateCookie(
                                name, value, domain, cookiePath);
                        cookie.IsHttpOnly = isHttpOnly;
                        cookie.IsSecure = isSecure;
                        cookie.SameSite = sameSite;
                        if (expiresBinary != 0)
                            cookie.Expires = expires;
                        webView.CoreWebView2.CookieManager
                            .AddOrUpdateCookie(cookie);
                        ++restored;
                    }
                }
                ClientDiagnosticLog.Write(
                    "spotify", "Restored " + restored +
                    " Spotify cookies from the encrypted user checkpoint.");
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "Could not restore the encrypted Spotify " +
                    "session checkpoint: " + error.Message);
            }
            await Task.CompletedTask;
        }

        private async Task SaveSpotifySessionAsync(string reason)
        {
            if (spotifySessionSaveActive || webView.CoreWebView2 == null)
                return;
            spotifySessionSaveActive = true;
            try
            {
                List<CoreWebView2Cookie> cookies =
                    await GetSpotifyCookiesAsync();
                if (cookies.Count == 0)
                    return;

                byte[] payload;
                using (var stream = new MemoryStream())
                {
                    using (var writer = new BinaryWriter(
                        stream, Encoding.UTF8, true))
                    {
                        writer.Write(SpotifySessionFormatVersion);
                        writer.Write(cookies.Count);
                        foreach (CoreWebView2Cookie cookie in cookies)
                        {
                            writer.Write(cookie.Name ?? "");
                            writer.Write(cookie.Value ?? "");
                            writer.Write(cookie.Domain ?? "");
                            writer.Write(cookie.Path ?? "/");
                            writer.Write(cookie.IsSession
                                ? 0L
                                : cookie.Expires.ToUniversalTime().ToBinary());
                            writer.Write(cookie.IsHttpOnly);
                            writer.Write(cookie.IsSecure);
                            writer.Write((int)cookie.SameSite);
                        }
                    }
                    payload = stream.ToArray();
                }

                byte[] encrypted = ProtectedData.Protect(
                    payload, null, DataProtectionScope.CurrentUser);
                string path = SpotifySessionPath;
                string temporaryPath = path + ".tmp";
                File.WriteAllBytes(temporaryPath, encrypted);
                if (File.Exists(path))
                    File.Replace(temporaryPath, path, null, true);
                else
                    File.Move(temporaryPath, path);
                ClientDiagnosticLog.Write(
                    "spotify", "Saved " + cookies.Count +
                    " Spotify cookies to the encrypted user checkpoint (" +
                    reason + ").");
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "Could not save the encrypted Spotify session " +
                    "checkpoint: " + error.Message);
            }
            finally
            {
                spotifySessionSaveActive = false;
            }
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
                SafeUriForLog(target));
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

        private async Task<int> TrySpotifyDomCommandAsync(string command)
        {
            string script;
            switch (command)
            {
                case "playpause":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-playpause\"]'); if(!b||b.disabled||b.getAttribute('aria-disabled')==='true')return 0; const s=window.prismGetPlaybackState?window.prismGetPlaybackState():(navigator.mediaSession&&navigator.mediaSession.playbackState)||'unknown'; b.click(); return s==='playing'?2:(s==='paused'?3:1); })()";
                    break;
                case "play":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-playpause\"]'); if(!b||b.disabled||b.getAttribute('aria-disabled')==='true')return 0; const s=window.prismGetPlaybackState?window.prismGetPlaybackState():(navigator.mediaSession&&navigator.mediaSession.playbackState)||'unknown'; if(s==='playing')return 3; const a=(b.getAttribute('aria-label')||'').toLowerCase(); if(s==='paused'||a.includes('play')){b.click();return 3;} return 0; })()";
                    break;
                case "pause":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-playpause\"]'); if(!b||b.disabled||b.getAttribute('aria-disabled')==='true')return 0; const s=window.prismGetPlaybackState?window.prismGetPlaybackState():(navigator.mediaSession&&navigator.mediaSession.playbackState)||'unknown'; if(s==='paused')return 2; const a=(b.getAttribute('aria-label')||'').toLowerCase(); if(s==='playing'||a.includes('pause')){b.click();return 2;} return 0; })()";
                    break;
                case "next":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-skip-forward\"]'); if(!b||b.disabled||b.getAttribute('aria-disabled')==='true')return 0; b.click(); return 1; })()";
                    break;
                case "previous":
                    script = "(() => { const b=document.querySelector('[data-testid=\"control-button-skip-back\"]'); if(!b||b.disabled||b.getAttribute('aria-disabled')==='true')return 0; b.click(); return 1; })()";
                    break;
                case "mute":
                    script = "(() => { const b=document.querySelector('[data-testid=\"volume-bar-toggle-mute-button\"]'); if(!b||b.disabled)return 0; b.click(); return 1; })()";
                    break;
                case "volumeup":
                case "volumedown":
                {
                    int direction = command == "volumeup" ? 1 : -1;
                    script = "(() => { const d=(" + direction + ")*0.05; const s=document.querySelector('input[data-testid=\"volume-bar\"], [data-testid=\"volume-bar\"] input, input[type=\"range\"][aria-label*=\"volume\" i]'); if(!s)return window.prismAdjustObservedVolume&&window.prismAdjustObservedVolume(d)?1:0; const lo=Number(s.min||0),hi=Number(s.max||1),step=(hi-lo)*0.05,next=Math.max(lo,Math.min(hi,Number(s.value)+(" + direction + ")*step)); const p=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value'); if(!p||!p.set)return 0; p.set.call(s,next); s.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'insertReplacementText',data:String(next)})); s.dispatchEvent(new Event('change',{bubbles:true})); return 1; })()";
                    break;
                }
                default:
                    return 0;
            }

            try
            {
                string result = await webView.CoreWebView2.ExecuteScriptAsync(script);
                int handled = 0;
                int.TryParse(
                    result, NumberStyles.Integer,
                    CultureInfo.InvariantCulture, out handled);
                ClientDiagnosticLog.Write(
                    "spotify", "DOM command " + command +
                    (handled != 0
                        ? " succeeded (state=" + handled + ")."
                        : " did not find an enabled control."));
                return handled;
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "Spotify DOM command " + command +
                    " failed: " + error.Message);
                return 0;
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
                if (!vehiclePowered)
                {
                    userWantsPlayback = !userWantsPlayback;
                    name = "pause";
                }
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
            int handled = await TrySpotifyDomCommandAsync(name);
            if (handled == 0)
            {
                // Spotify replaces its controls during track transitions.
                // One short internal retry avoids requiring another physical
                // key press without ever duplicating a successful click.
                await Task.Delay(120);
                handled = await TrySpotifyDomCommandAsync(name);
            }
            if (handled != 0)
            {
                if (handled == 2)
                    userWantsPlayback = false;
                else if (handled == 3)
                    userWantsPlayback = true;
                return;
            }

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

        private async Task LoadTrafficLocalFileAsync(string value)
        {
            try
            {
                string path = (value ?? "").Trim().Trim('"');
                if (Uri.TryCreate(path, UriKind.Absolute, out Uri fileUri) &&
                    fileUri.IsFile)
                {
                    path = fileUri.LocalPath;
                }
                path = Path.GetFullPath(
                    Environment.ExpandEnvironmentVariables(path));
                if (!File.Exists(path))
                {
                    ClientDiagnosticLog.Write(
                        "error", "Traffic audio file was not found: " +
                        Path.GetFileName(path));
                    return;
                }

                string folder = Path.GetDirectoryName(path);
                webView.CoreWebView2.SetVirtualHostNameToFolderMapping(
                    TrafficLocalHost, folder,
                    CoreWebView2HostResourceAccessKind.Allow);
                string target = "https://" + TrafficLocalHost + "/" +
                    Uri.EscapeDataString(Path.GetFileName(path));
                ClientDiagnosticLog.Write(
                    "traffic-audio", "Loading local traffic source: " +
                    Path.GetFileName(path));
                if (fullSpotifyWeb)
                    await NavigateToLocalPlayerAsync("load|" + target);
                else
                    await ExecuteCommandAsync("load|" + target);
            }
            catch (Exception error)
            {
                ClientDiagnosticLog.Write(
                    "error", "Could not load local traffic audio: " +
                    error.Message);
            }
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
            if (string.Equals(command, "shutdown", StringComparison.Ordinal))
            {
                Close();
                return;
            }
            if (command.StartsWith("brightness|", StringComparison.Ordinal))
            {
                if (!double.TryParse(
                    command.Substring(11), NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out desiredBrightness))
                    desiredBrightness = 1.0;
                desiredBrightness = Math.Max(
                    0.05, Math.Min(2.0, desiredBrightness));
                await ApplyBrightnessAsync("plugin brightness command");
                return;
            }
            if (string.Equals(
                    command, "volumeup", StringComparison.Ordinal) ||
                string.Equals(
                    command, "volumedown", StringComparison.Ordinal))
            {
                // Web-player sliders are framework-owned and Spotify can use
                // protected media elements. Adjust the WebView audio session
                // directly so every integrated player follows the command.
                adaptiveAudio.AdjustUserVolume(
                    command == "volumeup" ? 0.05f : -0.05f);
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
            if (command.StartsWith("randomize|", StringComparison.Ordinal))
            {
                await ExecuteCommandAsync(command);
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
                await SaveSpotifySessionAsync("helper returned to silent mode");
                SetInteractiveWindow(false);
                return;
            }
            if (string.Equals(command, "clearspotify", StringComparison.Ordinal))
            {
                await webView.CoreWebView2.Profile.ClearBrowsingDataAsync();
                if (File.Exists(SpotifySessionPath))
                    File.Delete(SpotifySessionPath);
                ClientDiagnosticLog.Write(
                    "spotify", "Persistent Spotify/WebView session and " +
                    "encrypted checkpoint cleared.");
                await NavigateToLocalPlayerAsync(null);
                return;
            }
            if (command.StartsWith("loadspotifyweb|", StringComparison.Ordinal))
            {
                await NavigateToFullSpotifyAsync(command.Substring(15));
                return;
            }
            if (command.StartsWith("loadlocal|", StringComparison.Ordinal))
            {
                await LoadTrafficLocalFileAsync(command.Substring(10));
                return;
            }
            if (command.StartsWith("loadmedia|", StringComparison.Ordinal) ||
                command.StartsWith("load|", StringComparison.Ordinal))
            {
                int prefixLength = command.StartsWith(
                    "loadmedia|", StringComparison.Ordinal) ? 10 : 5;
                string value = command.Substring(prefixLength);
                if (IsSpotifyValue(value))
                {
                    await NavigateToFullSpotifyAsync(value);
                }
                else if (fullSpotifyWeb)
                {
                    await NavigateToLocalPlayerAsync("load|" + value);
                }
                else
                {
                    await ExecuteCommandAsync("load|" + value);
                }
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
