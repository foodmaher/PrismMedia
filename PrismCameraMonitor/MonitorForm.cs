using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace PrismCameraMonitor
{
    internal sealed class MonitorForm : Form
    {
        private const string MappingName =
            "Local\\PrismTextureStreamerIndependentCameraV1";
        private const uint Magic = 0x50434D31;
        private const uint Version = 1;
        private const long RequestSequenceOffset = 64;
        private const long PixelsOffset = 744;
        private const int MaximumPixelBytes = 512 * 512 * 4;

        private readonly Label connectionLabel = new Label();
        private readonly Label stageLabel = new Label();
        private readonly Label countersLabel = new Label();
        private readonly TextBox detailBox = new TextBox();
        private readonly PictureBox preview = new PictureBox();
        private readonly ListBox timeline = new ListBox();
        private readonly Button runButton = new Button();
        private readonly Timer timer = new Timer();

        private MemoryMappedFile mapping;
        private MemoryMappedViewAccessor view;
        private ulong lastOpenAttempt;
        private ulong lastFrameSequence;
        private ulong lastRunId;
        private uint lastStage = uint.MaxValue;
        private int requestSequence;

        [DllImport("kernel32.dll")]
        private static extern ulong GetTickCount64();

        internal MonitorForm()
        {
            Text = "Prism Independent Camera Lab";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(900, 650);
            Size = new Size(1100, 760);
            BackColor = Color.FromArgb(24, 27, 33);
            ForeColor = Color.WhiteSmoke;
            Font = new Font("Segoe UI", 9F);

            var root = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 2,
                RowCount = 4,
                Padding = new Padding(12)
            };
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 66F));
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 34F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 38F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 54F));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 112F));
            Controls.Add(root);

            connectionLabel.Dock = DockStyle.Fill;
            connectionLabel.TextAlign = ContentAlignment.MiddleLeft;
            connectionLabel.Font = new Font(Font, FontStyle.Bold);
            connectionLabel.Text = "Waiting for game plugin…";
            root.Controls.Add(connectionLabel, 0, 0);

            runButton.Dock = DockStyle.Fill;
            runButton.Text = "Start new diagnostic";
            runButton.FlatStyle = FlatStyle.Flat;
            runButton.BackColor = Color.FromArgb(50, 95, 150);
            runButton.ForeColor = Color.White;
            runButton.Click += (_, __) => RequestDiagnostic();
            root.Controls.Add(runButton, 1, 0);

            var statusPanel = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 1,
                RowCount = 2
            };
            stageLabel.Dock = DockStyle.Fill;
            stageLabel.Font = new Font(Font.FontFamily, 12F, FontStyle.Bold);
            countersLabel.Dock = DockStyle.Fill;
            statusPanel.Controls.Add(stageLabel, 0, 0);
            statusPanel.Controls.Add(countersLabel, 0, 1);
            root.Controls.Add(statusPanel, 0, 1);
            root.SetColumnSpan(statusPanel, 2);

            preview.Dock = DockStyle.Fill;
            preview.SizeMode = PictureBoxSizeMode.Zoom;
            preview.BackColor = Color.Black;
            preview.BorderStyle = BorderStyle.FixedSingle;
            root.Controls.Add(preview, 0, 2);

            timeline.Dock = DockStyle.Fill;
            timeline.BackColor = Color.FromArgb(17, 19, 24);
            timeline.ForeColor = Color.Gainsboro;
            timeline.BorderStyle = BorderStyle.FixedSingle;
            root.Controls.Add(timeline, 1, 2);

            detailBox.Dock = DockStyle.Fill;
            detailBox.Multiline = true;
            detailBox.ReadOnly = true;
            detailBox.ScrollBars = ScrollBars.Vertical;
            detailBox.BackColor = Color.FromArgb(17, 19, 24);
            detailBox.ForeColor = Color.Gainsboro;
            detailBox.BorderStyle = BorderStyle.FixedSingle;
            root.Controls.Add(detailBox, 0, 3);
            root.SetColumnSpan(detailBox, 2);

            timer.Interval = 50;
            timer.Tick += (_, __) => Poll();
            timer.Start();
            FormClosed += (_, __) => CloseMapping();
        }

        private static string StageName(uint stage)
        {
            switch (stage)
            {
                case 0: return "Offline";
                case 1: return "Plugin ready";
                case 2: return "Diagnostic started";
                case 3: return "Slot 7 blocked";
                case 4: return "Discovering independent camera state";
                case 5: return "Independent camera state ready";
                case 6: return "Plugin-owned render target ready";
                case 7: return "Independent render job submitted";
                case 8: return "Independent renderer entered";
                case 9: return "Unique render target tagged";
                case 10: return "Readback pending";
                case 11: return "Verified frame ready";
                case 12: return "Blocked";
                case 13: return "Failed";
                default: return "Unknown stage " + stage;
            }
        }

        private void TryOpenMapping()
        {
            ulong now = GetTickCount64();
            if (lastOpenAttempt != 0 && now - lastOpenAttempt < 500)
                return;
            lastOpenAttempt = now;
            try
            {
                mapping = MemoryMappedFile.OpenExisting(
                    MappingName, MemoryMappedFileRights.ReadWrite);
                view = mapping.CreateViewAccessor(
                    0, 0, MemoryMappedFileAccess.ReadWrite);
                requestSequence = view.ReadInt32(RequestSequenceOffset);
            }
            catch
            {
                CloseMapping();
            }
        }

        private void CloseMapping()
        {
            view?.Dispose();
            mapping?.Dispose();
            view = null;
            mapping = null;
        }

        private string ReadAscii(long offset, int capacity)
        {
            var bytes = new byte[capacity];
            view.ReadArray(offset, bytes, 0, bytes.Length);
            int length = Array.IndexOf(bytes, (byte)0);
            if (length < 0)
                length = bytes.Length;
            return Encoding.UTF8.GetString(bytes, 0, length);
        }

        private void Poll()
        {
            if (view == null)
            {
                TryOpenMapping();
                if (view == null)
                {
                    connectionLabel.Text = "Waiting for game plugin…";
                    connectionLabel.ForeColor = Color.Goldenrod;
                    runButton.Enabled = false;
                    return;
                }
            }

            try
            {
                int before = view.ReadInt32(8);
                if ((before & 1) != 0)
                    return;
                uint magic = view.ReadUInt32(0);
                uint version = view.ReadUInt32(4);
                uint stage = view.ReadUInt32(12);
                uint flags = view.ReadUInt32(16);
                int error = view.ReadInt32(20);
                ulong updatedTick = view.ReadUInt64(24);
                ulong runId = view.ReadUInt64(32);
                ulong frameSequence = view.ReadUInt64(40);
                uint width = view.ReadUInt32(48);
                uint height = view.ReadUInt32(52);
                uint stride = view.ReadUInt32(56);
                uint pixelBytes = view.ReadUInt32(60);
                ulong observedJobs = view.ReadUInt64(72);
                ulong rejectedSlot7 = view.ReadUInt64(80);
                ulong taggedTargets = view.ReadUInt64(88);
                ulong readbackFrames = view.ReadUInt64(96);
                string stageText = ReadAscii(104, 128);
                string detail = ReadAscii(232, 512);
                ulong requiredBytes = height == 0
                    ? 0
                    : (ulong)(height - 1) * stride + (ulong)width * 4;
                bool frameAvailable = (flags & (1U << 4)) != 0;
                bool newRun = runId != lastRunId;
                bool readNewFrame = frameAvailable &&
                    (newRun || frameSequence != lastFrameSequence) &&
                    width > 0 && height > 0 && width <= 512 && height <= 512 &&
                    stride >= width * 4 && pixelBytes <= MaximumPixelBytes &&
                    requiredBytes <= pixelBytes;
                byte[] framePixels = null;
                if (readNewFrame)
                {
                    framePixels = new byte[checked((int)pixelBytes)];
                    view.ReadArray(
                        PixelsOffset, framePixels, 0, framePixels.Length);
                }
                int after = view.ReadInt32(8);
                if (before != after || (after & 1) != 0)
                    return;

                if (magic != Magic || version != Version)
                    throw new InvalidOperationException("Protocol mismatch");

                ulong now = GetTickCount64();
                ulong age = now >= updatedTick ? now - updatedTick : 0;
                bool connected = (flags & 1U) != 0 && age < 2000;
                connectionLabel.Text = connected
                    ? "Connected — slot 7 is disabled"
                    : "Plugin channel is stale/offline";
                connectionLabel.ForeColor = connected
                    ? Color.LightGreen
                    : Color.OrangeRed;
                runButton.Enabled = connected;
                stageLabel.Text = StageName(stage) +
                    (string.IsNullOrWhiteSpace(stageText)
                        ? ""
                        : " — " + stageText);
                stageLabel.ForeColor = stage == 11
                    ? Color.LightGreen
                    : stage >= 12
                        ? Color.OrangeRed
                        : Color.LightSkyBlue;
                countersLabel.Text = string.Format(
                    "Run {0} | Jobs observed: {1} | Slot-7 jobs rejected: {2} | " +
                    "Unique targets: {3} | Readback frames: {4} | Error: {5}",
                    runId, observedJobs, rejectedSlot7, taggedTargets,
                    readbackFrames, error);
                detailBox.Text = detail;

                if (runId != lastRunId || stage != lastStage)
                {
                    timeline.Items.Insert(0,
                        DateTime.Now.ToString("HH:mm:ss.fff") + "  " +
                        StageName(stage) +
                        (string.IsNullOrWhiteSpace(stageText)
                            ? ""
                            : ": " + stageText));
                    while (timeline.Items.Count > 100)
                        timeline.Items.RemoveAt(timeline.Items.Count - 1);
                    lastRunId = runId;
                    lastStage = stage;
                }

                if (readNewFrame)
                {
                    UpdatePreview(framePixels, width, height, stride);
                    lastFrameSequence = frameSequence;
                }
            }
            catch (Exception ex)
            {
                connectionLabel.Text = "Monitor read failed";
                connectionLabel.ForeColor = Color.OrangeRed;
                detailBox.Text = "Viewer IPC error: " + ex.Message;
                CloseMapping();
            }
        }

        private void UpdatePreview(
            byte[] source, uint width, uint height, uint stride)
        {
            var bitmap = new Bitmap(
                (int)width, (int)height, PixelFormat.Format32bppArgb);
            BitmapData data = bitmap.LockBits(
                new Rectangle(0, 0, bitmap.Width, bitmap.Height),
                ImageLockMode.WriteOnly,
                PixelFormat.Format32bppArgb);
            try
            {
                int destinationStride = Math.Abs(data.Stride);
                int rowBytes = checked((int)width * 4);
                for (int row = 0; row < (int)height; ++row)
                {
                    Marshal.Copy(
                        source,
                        checked(row * (int)stride),
                        IntPtr.Add(data.Scan0, row * destinationStride),
                        rowBytes);
                }
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
            Image previous = preview.Image;
            preview.Image = bitmap;
            previous?.Dispose();
        }

        private void RequestDiagnostic()
        {
            if (view == null)
                return;
            try
            {
                requestSequence = unchecked(requestSequence + 1);
                view.Write(RequestSequenceOffset, requestSequence);
                view.Flush();
                detailBox.Text = "Diagnostic requested; waiting for the plugin.";
            }
            catch (Exception ex)
            {
                detailBox.Text = "Could not request diagnosis: " + ex.Message;
                CloseMapping();
            }
        }
    }
}
