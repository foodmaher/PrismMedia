using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;

namespace PrismMediaClient
{
    internal static class ClientDiagnosticLog
    {
        private const long MaximumLogBytes = 4L * 1024L * 1024L;
        private static readonly BlockingCollection<string> Lines =
            new BlockingCollection<string>(256);
        private static Thread writer;
        private static string logPath;

        internal static void Start(string clientDirectory)
        {
            try
            {
                string directory = clientDirectory;
                if (string.IsNullOrWhiteSpace(directory))
                {
                    directory = Path.Combine(
                        Environment.GetFolderPath(
                            Environment.SpecialFolder.LocalApplicationData),
                        "PrismMedia", "Clients", "display");
                }
                Directory.CreateDirectory(directory);
                logPath = Path.Combine(directory, "PrismMediaClient.log");
                writer = new Thread(WriterLoop)
                {
                    IsBackground = true,
                    Name = "Prism media diagnostic log",
                    Priority = ThreadPriority.BelowNormal
                };
                writer.Start();
                Write("session", "PrismMediaClient started.");
            }
            catch
            {
                // Diagnostics are best effort and never block playback.
            }
        }

        internal static void Write(string category, string message)
        {
            if (writer == null || Lines.IsAddingCompleted ||
                string.IsNullOrEmpty(message))
                return;
            Lines.TryAdd(
                (category ?? "info") + "\t" + message);
        }

        internal static void Stop()
        {
            if (writer == null)
                return;
            Write("session", "PrismMediaClient stopped.");
            Lines.CompleteAdding();
            writer.Join(2000);
            writer = null;
        }

        private static void WriterLoop()
        {
            try
            {
                if (File.Exists(logPath) &&
                    new FileInfo(logPath).Length >= MaximumLogBytes)
                {
                    string previous = logPath + ".old";
                    if (File.Exists(previous))
                        File.Delete(previous);
                    File.Move(logPath, previous);
                }

                using (var stream = new FileStream(
                    logPath, FileMode.Append, FileAccess.Write,
                    FileShare.ReadWrite | FileShare.Delete))
                using (var output = new StreamWriter(
                    stream, new UTF8Encoding(false)))
                {
                    output.AutoFlush = true;
                    foreach (string line in Lines.GetConsumingEnumerable())
                    {
                        int separator = line.IndexOf('\t');
                        string category = separator < 0
                            ? "info" : line.Substring(0, separator);
                        string message = separator < 0
                            ? line : line.Substring(separator + 1);
                        output.WriteLine(
                            DateTime.Now.ToString(
                                "yyyy-MM-dd HH:mm:ss.fff",
                                CultureInfo.InvariantCulture) +
                            " [" + category + "] " + message);
                    }
                    output.Flush();
                }
            }
            catch
            {
                // A read-only game directory must not affect the media client.
            }
        }
    }
}
