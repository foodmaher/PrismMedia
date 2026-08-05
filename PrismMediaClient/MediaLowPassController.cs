using Microsoft.Web.WebView2.Core;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading.Tasks;

namespace PrismMediaClient
{
    // Applies Web Audio filtering inside the document that owns each media
    // element, including the cross-origin YouTube iframe. It waits for an
    // AudioContext to be running before routing a media element through it,
    // so a browser autoplay restriction cannot turn working audio silent.
    internal sealed class MediaLowPassController : IDisposable
    {
        private readonly CoreWebView2 core;
        private readonly List<CoreWebView2Frame> frames =
            new List<CoreWebView2Frame>();
        private float cutoffHz = 20000.0f;
        private bool enabled;
        private bool disposed;
        private bool lastLoggedEnabled;
        private float lastLoggedCutoffHz = 20000.0f;
        private DateTime lastCommandLogUtc = DateTime.MinValue;

        internal MediaLowPassController(CoreWebView2 webViewCore)
        {
            core = webViewCore;
            core.FrameCreated += OnFrameCreated;
            core.NavigationCompleted += OnTopLevelNavigationCompleted;
        }

        internal async Task InstallAsync(string bootstrapScript)
        {
            await core.AddScriptToExecuteOnDocumentCreatedAsync(
                bootstrapScript);
        }

        internal void SetDesired(bool spatialEnabled, float desiredCutoffHz)
        {
            cutoffHz = Math.Max(
                20.0f, Math.Min(20000.0f, desiredCutoffHz));
            enabled = spatialEnabled && cutoffHz < 19500.0f;
            DateTime now = DateTime.UtcNow;
            bool stateChanged = enabled != lastLoggedEnabled;
            bool cutoffChanged = Math.Abs(
                cutoffHz - lastLoggedCutoffHz) >= 35.0f;
            if (stateChanged ||
                (cutoffChanged &&
                    (now - lastCommandLogUtc).TotalMilliseconds >= 750.0))
            {
                ClientDiagnosticLog.Write(
                    "audio", "Low-pass command received: enabled=" +
                    enabled + ", cutoff=" + cutoffHz.ToString(
                        "0.0", CultureInfo.InvariantCulture) + " Hz.");
                lastLoggedEnabled = enabled;
                lastLoggedCutoffHz = cutoffHz;
                lastCommandLogUtc = now;
            }
            _ = ApplyEverywhereAsync();
        }

        private void OnFrameCreated(
            object sender,
            CoreWebView2FrameCreatedEventArgs args)
        {
            TrackFrame(args.Frame);
        }

        private async void OnTopLevelNavigationCompleted(
            object sender,
            CoreWebView2NavigationCompletedEventArgs args)
        {
            if (disposed || !args.IsSuccess)
                return;

            // The bootstrap runs in every new document with safe defaults.
            // YouTube is hosted in a child frame and was already refreshed by
            // TrackFrame(). Full Spotify Web is the top-level document, so it
            // also needs the current cutoff reapplied after every navigation.
            await ApplyEverywhereAsync();
            ClientDiagnosticLog.Write(
                "audio", "Adaptive low-pass state reapplied to the " +
                "top-level media document (enabled=" + enabled +
                ", cutoff=" + cutoffHz.ToString(
                    "0.0", CultureInfo.InvariantCulture) + " Hz).");
        }

        private void TrackFrame(CoreWebView2Frame frame)
        {
            if (frame == null || disposed)
                return;

            frames.Add(frame);
            frame.FrameCreated += OnFrameCreated;
            frame.NavigationCompleted += async (_, __) =>
                await ApplyToFrameAsync(frame);
            frame.Destroyed += (_, __) => frames.Remove(frame);
            _ = ApplyToFrameAsync(frame);
        }

        private string BuildApplyScript()
        {
            string cutoff = cutoffHz.ToString(
                "0.0", CultureInfo.InvariantCulture);
            return "if (window.prismSetLowPass) {" +
                "window.prismSetLowPass(" + cutoff + "," +
                (enabled ? "true" : "false") + ");}";
        }

        private async Task ApplyEverywhereAsync()
        {
            if (disposed)
                return;

            string script = BuildApplyScript();
            try
            {
                await core.ExecuteScriptAsync(script);
            }
            catch
            {
                // Navigation can replace a document between scheduling and
                // execution. NavigationCompleted retries with the new one.
            }

            foreach (CoreWebView2Frame frame in frames.ToArray())
                await ApplyToFrameAsync(frame, script);
        }

        private Task ApplyToFrameAsync(CoreWebView2Frame frame)
        {
            return ApplyToFrameAsync(frame, BuildApplyScript());
        }

        private static async Task ApplyToFrameAsync(
            CoreWebView2Frame frame,
            string script)
        {
            try
            {
                // WebView2 exposes IsDestroyed as an HRESULT-style integer in
                // the net462/net48 projection used by the Windows runner.
                if (frame != null && frame.IsDestroyed() == 0)
                    await frame.ExecuteScriptAsync(script);
            }
            catch
            {
                // Frames are short lived during YouTube navigation.
            }
        }

        public void Dispose()
        {
            disposed = true;
            core.FrameCreated -= OnFrameCreated;
            core.NavigationCompleted -= OnTopLevelNavigationCompleted;
            frames.Clear();
        }
    }
}
