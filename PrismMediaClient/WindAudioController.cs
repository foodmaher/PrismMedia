using System;
using System.IO;
using System.Windows.Media;

namespace PrismMediaClient
{
    /// <summary>
    /// Plays window wind outside WebView2, so it is not affected by media
    /// spatial-audio filtering and never touches the game's render thread.
    /// </summary>
    internal sealed class WindAudioController : IDisposable
    {
        private readonly MediaPlayer player = new MediaPlayer();
        private bool desiredPlaying;
        private double desiredVolume;
        private double desiredBalance;
        private double desiredSpeedRatio = 1.0;
        private double appliedVolume = double.NaN;
        private double appliedBalance = double.NaN;
        private double appliedSpeedRatio = double.NaN;
        private bool appliedPlaying;
        private bool customSource;
        private bool fallbackAttempted;
        private bool disposed;

        internal WindAudioController()
        {
            player.MediaOpened += (_, __) =>
            {
                ResetAppliedState();
                ApplyState();
            };
            player.MediaEnded += (_, __) =>
            {
                if (!desiredPlaying)
                    return;
                player.Position = TimeSpan.Zero;
                player.Play();
                appliedPlaying = true;
            };
            player.MediaFailed += (_, __) =>
            {
                if (customSource && !fallbackAttempted)
                {
                    fallbackAttempted = true;
                    customSource = false;
                    player.Close();
                    ResetAppliedState();
                    player.Open(new Uri(
                        EnsureProceduralWindFile(), UriKind.Absolute));
                    ApplyState();
                    return;
                }
                desiredPlaying = false;
                player.Stop();
                appliedPlaying = false;
            };
        }

        internal void SetSource(bool procedural, string customPath)
        {
            if (disposed)
                return;

            string path = procedural
                ? EnsureProceduralWindFile()
                : customPath?.Trim().Trim('"');
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            {
                path = EnsureProceduralWindFile();
                procedural = true;
            }

            customSource = !procedural;
            fallbackAttempted = false;
            player.Close();
            ResetAppliedState();
            player.Open(new Uri(Path.GetFullPath(path), UriKind.Absolute));
            ApplyState();
        }

        internal void SetState(
            bool enabled,
            float volume,
            float pan,
            float speedBlend)
        {
            if (disposed)
                return;

            desiredPlaying = enabled && volume > 0.0005f;
            desiredVolume = Math.Max(0.0, Math.Min(1.0, volume));
            desiredBalance = Math.Max(-1.0, Math.Min(1.0, pan));
            desiredSpeedRatio = 0.82 +
                Math.Max(0.0, Math.Min(1.0, speedBlend)) * 0.36;
            ApplyState();
        }

        private void ApplyState()
        {
            if (double.IsNaN(appliedVolume) ||
                Math.Abs(appliedVolume - desiredVolume) >= 0.003)
            {
                player.Volume = desiredVolume;
                appliedVolume = desiredVolume;
            }
            if (double.IsNaN(appliedBalance) ||
                Math.Abs(appliedBalance - desiredBalance) >= 0.01)
            {
                player.Balance = desiredBalance;
                appliedBalance = desiredBalance;
            }
            if (double.IsNaN(appliedSpeedRatio) ||
                Math.Abs(appliedSpeedRatio - desiredSpeedRatio) >= 0.01)
            {
                player.SpeedRatio = desiredSpeedRatio;
                appliedSpeedRatio = desiredSpeedRatio;
            }
            if (desiredPlaying == appliedPlaying)
                return;
            if (desiredPlaying)
                player.Play();
            else
                player.Pause();
            appliedPlaying = desiredPlaying;
        }

        private void ResetAppliedState()
        {
            appliedVolume = double.NaN;
            appliedBalance = double.NaN;
            appliedSpeedRatio = double.NaN;
            appliedPlaying = false;
        }

        private static string EnsureProceduralWindFile()
        {
            string directory = Path.Combine(
                Environment.GetFolderPath(
                    Environment.SpecialFolder.LocalApplicationData),
                "PrismTextureStreamerFB");
            Directory.CreateDirectory(directory);
            string path = Path.Combine(directory, "procedural-window-wind-v1.wav");
            if (File.Exists(path) && new FileInfo(path).Length > 100000)
                return path;

            const int sampleRate = 22050;
            const int seconds = 8;
            const short channels = 1;
            const short bitsPerSample = 16;
            int sampleCount = sampleRate * seconds;
            int dataLength = sampleCount * channels * (bitsPerSample / 8);
            var random = new Random(0x50524953);
            double low = 0.0;
            double mid = 0.0;

            using (var stream = File.Create(path))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write(new[] { 'R', 'I', 'F', 'F' });
                writer.Write(36 + dataLength);
                writer.Write(new[] { 'W', 'A', 'V', 'E' });
                writer.Write(new[] { 'f', 'm', 't', ' ' });
                writer.Write(16);
                writer.Write((short)1);
                writer.Write(channels);
                writer.Write(sampleRate);
                writer.Write(sampleRate * channels * (bitsPerSample / 8));
                writer.Write((short)(channels * (bitsPerSample / 8)));
                writer.Write(bitsPerSample);
                writer.Write(new[] { 'd', 'a', 't', 'a' });
                writer.Write(dataLength);

                for (int index = 0; index < sampleCount; ++index)
                {
                    double white = random.NextDouble() * 2.0 - 1.0;
                    low = low * 0.995 + white * 0.005;
                    mid = mid * 0.93 + white * 0.07;
                    double time = index / (double)sampleRate;
                    double gust = 0.72 + 0.18 * Math.Sin(time * 1.7) +
                        0.10 * Math.Sin(time * 0.37 + 1.3);
                    double sample = (low * 2.7 + mid * 0.75 +
                        white * 0.07) * gust;
                    sample = Math.Max(-1.0, Math.Min(1.0, sample));
                    writer.Write((short)(sample * 24500.0));
                }
            }
            return path;
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
