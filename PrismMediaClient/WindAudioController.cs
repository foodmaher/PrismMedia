using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Media;

namespace PrismMediaClient
{
    /// <summary>
    /// Keeps three environment loops running continuously and changes only
    /// their volume and balance as the truck state changes.
    /// </summary>
    internal sealed class WindAudioController : IDisposable
    {
        private readonly LoopLayer stationary = new LoopLayer();
        private readonly LoopLayer city = new LoopLayer();
        private readonly LoopLayer highway = new LoopLayer();
        private bool disposed;

        internal WindAudioController()
        {
            try
            {
                string legacyNoise = Path.Combine(
                    Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData),
                    "PrismTextureStreamerFB",
                    "procedural-window-wind-v1.wav");
                if (File.Exists(legacyNoise))
                    File.Delete(legacyNoise);
            }
            catch
            {
                // A locked legacy file is harmless; it is never played again.
            }
        }

        internal void SetLibrary(string category, IEnumerable<string> files)
        {
            if (disposed)
                return;

            LoopLayer layer = LayerFor(category);
            if (layer != null)
                layer.SetFiles(files);
        }

        internal void SetState(
            bool enabled,
            float stationaryVolume,
            float cityVolume,
            float highwayVolume,
            float pan)
        {
            if (disposed)
                return;

            stationary.SetState(enabled, stationaryVolume, pan);
            city.SetState(enabled, cityVolume, pan);
            highway.SetState(enabled, highwayVolume, pan);
        }

        private LoopLayer LayerFor(string category)
        {
            if (string.Equals(category, "stationary",
                StringComparison.OrdinalIgnoreCase))
                return stationary;
            if (string.Equals(category, "city",
                StringComparison.OrdinalIgnoreCase))
                return city;
            if (string.Equals(category, "highway",
                StringComparison.OrdinalIgnoreCase))
                return highway;
            return null;
        }

        public void Dispose()
        {
            if (disposed)
                return;
            disposed = true;
            stationary.Dispose();
            city.Dispose();
            highway.Dispose();
        }

        private sealed class LoopLayer : IDisposable
        {
            private readonly MediaPlayer player = new MediaPlayer();
            private List<string> files = new List<string>();
            private int currentIndex = -1;
            private int consecutiveFailures;
            private bool desiredRunning;
            private double desiredVolume;
            private double desiredBalance;
            private double appliedVolume = double.NaN;
            private double appliedBalance = double.NaN;
            private bool mediaOpen;
            private bool appliedPlaying;
            private bool disposed;

            internal LoopLayer()
            {
                player.MediaOpened += (_, __) =>
                {
                    mediaOpen = true;
                    consecutiveFailures = 0;
                    ResetAppliedState();
                    ApplyState();
                };
                player.MediaEnded += (_, __) => AdvanceAfterEnd();
                player.MediaFailed += (_, __) => AdvanceAfterFailure();
            }

            internal void SetFiles(IEnumerable<string> values)
            {
                if (disposed)
                    return;

                var normalized = new List<string>();
                if (values != null)
                {
                    foreach (string value in values)
                    {
                        try
                        {
                            string path = (value ?? "").Trim().Trim('"');
                            if (string.IsNullOrWhiteSpace(path))
                                continue;
                            path = Path.GetFullPath(path);
                            if (File.Exists(path) && !normalized.Exists(
                                item => string.Equals(item, path,
                                    StringComparison.OrdinalIgnoreCase)))
                                normalized.Add(path);
                        }
                        catch
                        {
                            // Invalid optional entries are ignored.
                        }
                    }
                }
                if (SameFiles(files, normalized))
                    return;

                files = normalized;
                currentIndex = -1;
                consecutiveFailures = 0;
                player.Close();
                mediaOpen = false;
                ResetAppliedState();
                OpenNext();
            }

            private static bool SameFiles(
                IList<string> first, IList<string> second)
            {
                if (first.Count != second.Count)
                    return false;
                for (int index = 0; index < first.Count; ++index)
                {
                    if (!string.Equals(first[index], second[index],
                        StringComparison.OrdinalIgnoreCase))
                        return false;
                }
                return true;
            }

            internal void SetState(bool running, float volume, float balance)
            {
                if (disposed)
                    return;

                // Once a valid layer is configured it remains in playback,
                // including at zero volume. Window and speed changes therefore
                // never restart the decoder or jump the loop position.
                desiredRunning = running;
                desiredVolume = Math.Max(0.0, Math.Min(1.0, volume));
                desiredBalance = Math.Max(-1.0, Math.Min(1.0, balance));
                ApplyState();
            }

            private void ApplyState()
            {
                if (double.IsNaN(appliedVolume) ||
                    Math.Abs(appliedVolume - desiredVolume) >= 0.002)
                {
                    player.Volume = desiredVolume;
                    appliedVolume = desiredVolume;
                }
                if (double.IsNaN(appliedBalance) ||
                    Math.Abs(appliedBalance - desiredBalance) >= 0.005)
                {
                    player.Balance = desiredBalance;
                    appliedBalance = desiredBalance;
                }
                if (!mediaOpen || desiredRunning == appliedPlaying)
                    return;

                if (desiredRunning)
                    player.Play();
                else
                    player.Pause();
                appliedPlaying = desiredRunning;
            }

            private void AdvanceAfterEnd()
            {
                if (files.Count == 0)
                    return;

                if (files.Count == 1)
                {
                    player.Position = TimeSpan.Zero;
                    if (desiredRunning)
                    {
                        player.Play();
                        appliedPlaying = true;
                    }
                    return;
                }
                OpenNext();
            }

            private void AdvanceAfterFailure()
            {
                mediaOpen = false;
                appliedPlaying = false;
                ++consecutiveFailures;
                if (consecutiveFailures >= files.Count)
                {
                    player.Close();
                    return;
                }
                OpenNext();
            }

            private void OpenNext()
            {
                if (files.Count == 0 || disposed)
                    return;

                currentIndex = (currentIndex + 1) % files.Count;
                mediaOpen = false;
                appliedPlaying = false;
                player.Open(new Uri(files[currentIndex], UriKind.Absolute));
            }

            private void ResetAppliedState()
            {
                appliedVolume = double.NaN;
                appliedBalance = double.NaN;
                appliedPlaying = false;
            }

            public void Dispose()
            {
                if (disposed)
                    return;
                disposed = true;
                player.Stop();
                player.Close();
            }
        }
    }
}
