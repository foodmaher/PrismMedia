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

        internal MediaLowPassController(CoreWebView2 webViewCore)
        {
            core = webViewCore;
            core.FrameCreated += OnFrameCreated;
        }

        internal async Task InstallAsync(string bootstrapScript)
        {
            await core.AddScriptToExecuteOnDocumentCreatedAsync(
                bootstrapScript);
        }

        internal void SetDesired(bool spatialEnabled, float desiredCutoffHz)
        {
            cutoffHz = Math.Max(
                120.0f, Math.Min(20000.0f, desiredCutoffHz));
            enabled = spatialEnabled && cutoffHz < 19500.0f;
            _ = ApplyEverywhereAsync();
        }

        private void OnFrameCreated(
            object sender,
            CoreWebView2FrameCreatedEventArgs args)
        {
            TrackFrame(args.Frame);
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
                if (frame != null && !frame.IsDestroyed())
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
            frames.Clear();
        }
    }
}
