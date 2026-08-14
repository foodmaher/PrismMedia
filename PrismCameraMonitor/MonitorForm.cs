using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace PrismCameraMonitor
{
    internal sealed class MonitorForm : Form
    {
        private const string MappingName =
            "Local\\PrismTextureStreamerIndependentCameraV2";
        private const uint Magic = 0x50434D32;
        private const uint Version = 2;
        private const long RequestSequenceOffset = 64;
        private const long PhaseRequestSequenceOffset = 68;
        private const long RequestedPhaseOffset = 116;
        private const long CandidatesOffset = 1024;
        private const int CandidateSize = 64;
        private const long PixelsOffset = 2048;
        private const int MaximumPixelBytes = 512 * 512 * 4;

        private readonly Label connectionLabel = new Label();
        private readonly Label stageLabel = new Label();
        private readonly Label countersLabel = new Label();
        private readonly TextBox detailBox = new TextBox();
        private readonly PictureBox preview = new PictureBox();
        private readonly ListBox timeline = new ListBox();
        private readonly Button runButton = new Button();
        private readonly Button phaseButton = new Button();
        private readonly Button saveButton = new Button();
        private readonly Label instructionLabel = new Label();
        private readonly DataGridView candidatesGrid = new DataGridView();
        private readonly Timer timer = new Timer();

        private MemoryMappedFile mapping;
        private MemoryMappedViewAccessor view;
        private ulong lastOpenAttempt;
        private ulong lastFrameSequence;
        private ulong lastRunId;
        private ulong lastAutoSavedRun;
        private uint lastStage = uint.MaxValue;
        private int requestSequence;
        private int phaseRequestSequence;
        private uint currentPhase;
        private uint pendingPhase;
        private uint completedPhaseMask;
        private ulong correlationSamples;
        private ulong observedJobs;
        private ulong submittedProbeJobs;
        private string currentStageText = "";
        private string currentDetail = "";
        private string currentInstruction = "";
        private readonly List<CorrelationCandidateView> currentCandidates =
            new List<CorrelationCandidateView>();

        private sealed class CorrelationCandidateView
        {
            internal ulong Address;
            internal uint Offset;
            internal uint Source;
            internal float Score;
            internal uint ChangedMask;
            internal uint CapturedMask;
            internal uint Samples;
            internal readonly float[] Values = new float[4];
            internal string Label;
        }

        [DllImport("kernel32.dll")]
        private static extern ulong GetTickCount64();

        internal MonitorForm()
        {
            Text = "Prism Call-Path Trace";
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
                RowCount = 5,
                Padding = new Padding(12)
            };
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 66F));
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 34F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 38F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 54F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 92F));
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

            var guidancePanel = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 3,
                RowCount = 1,
                Padding = new Padding(0, 5, 0, 5)
            };
            guidancePanel.ColumnStyles.Add(
                new ColumnStyle(SizeType.Percent, 64F));
            guidancePanel.ColumnStyles.Add(
                new ColumnStyle(SizeType.Percent, 24F));
            guidancePanel.ColumnStyles.Add(
                new ColumnStyle(SizeType.Percent, 12F));
            instructionLabel.Dock = DockStyle.Fill;
            instructionLabel.TextAlign = ContentAlignment.MiddleLeft;
            instructionLabel.Padding = new Padding(8);
            instructionLabel.BackColor = Color.FromArgb(37, 42, 51);
            instructionLabel.Text =
                "Press Start once, remain in the truck for 10 seconds, then send the two trace files from Documents\\ETS2.";
            guidancePanel.Controls.Add(instructionLabel, 0, 0);

            phaseButton.Dock = DockStyle.Fill;
            phaseButton.Text = "Capture and continue";
            phaseButton.FlatStyle = FlatStyle.Flat;
            phaseButton.BackColor = Color.FromArgb(55, 105, 75);
            phaseButton.ForeColor = Color.White;
            phaseButton.Enabled = false;
            phaseButton.Visible = false;
            phaseButton.Click += (_, __) => RequestPhaseCapture();
            guidancePanel.Controls.Add(phaseButton, 1, 0);

            saveButton.Dock = DockStyle.Fill;
            saveButton.Text = "Save report";
            saveButton.FlatStyle = FlatStyle.Flat;
            saveButton.Enabled = false;
            saveButton.Visible = false;
            saveButton.Click += (_, __) => SaveReport(true);
            guidancePanel.Controls.Add(saveButton, 2, 0);
            root.Controls.Add(guidancePanel, 0, 2);
            root.SetColumnSpan(guidancePanel, 2);

            preview.Dock = DockStyle.Fill;
            preview.SizeMode = PictureBoxSizeMode.Zoom;
            preview.BackColor = Color.Black;
            preview.BorderStyle = BorderStyle.FixedSingle;
            root.Controls.Add(preview, 0, 3);

            timeline.Dock = DockStyle.Fill;
            timeline.BackColor = Color.FromArgb(17, 19, 24);
            timeline.ForeColor = Color.Gainsboro;
            timeline.BorderStyle = BorderStyle.FixedSingle;
            var tabs = new TabControl { Dock = DockStyle.Fill };
            var candidatesPage = new TabPage("Candidates");
            var timelinePage = new TabPage("Timeline");
            candidatesPage.BackColor = Color.FromArgb(17, 19, 24);
            timelinePage.BackColor = Color.FromArgb(17, 19, 24);
            candidatesGrid.Dock = DockStyle.Fill;
            candidatesGrid.ReadOnly = true;
            candidatesGrid.AllowUserToAddRows = false;
            candidatesGrid.AllowUserToDeleteRows = false;
            candidatesGrid.AllowUserToResizeRows = false;
            candidatesGrid.RowHeadersVisible = false;
            candidatesGrid.AutoSizeColumnsMode =
                DataGridViewAutoSizeColumnsMode.DisplayedCells;
            candidatesGrid.BackgroundColor = Color.FromArgb(17, 19, 24);
            candidatesGrid.ForeColor = Color.Gainsboro;
            candidatesGrid.DefaultCellStyle.BackColor =
                Color.FromArgb(24, 27, 33);
            candidatesGrid.DefaultCellStyle.SelectionBackColor =
                Color.FromArgb(50, 95, 150);
            candidatesGrid.Columns.Add("Rank", "#");
            candidatesGrid.Columns.Add("Label", "Type");
            candidatesGrid.Columns.Add("Path", "Reproducible path");
            candidatesGrid.Columns.Add("Address", "Address + offset");
            candidatesGrid.Columns.Add("Score", "Score");
            candidatesGrid.Columns.Add("Changed", "Changed phases");
            candidatesGrid.Columns.Add("Values", "Latest float4");
            candidatesPage.Controls.Add(candidatesGrid);
            timelinePage.Controls.Add(timeline);
            tabs.TabPages.Add(candidatesPage);
            tabs.TabPages.Add(timelinePage);
            root.Controls.Add(tabs, 1, 3);

            detailBox.Dock = DockStyle.Fill;
            detailBox.Multiline = true;
            detailBox.ReadOnly = true;
            detailBox.ScrollBars = ScrollBars.Vertical;
            detailBox.BackColor = Color.FromArgb(17, 19, 24);
            detailBox.ForeColor = Color.Gainsboro;
            detailBox.BorderStyle = BorderStyle.FixedSingle;
            root.Controls.Add(detailBox, 0, 4);
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
                case 3: return "GPU trace ready";
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
                case 14: return "Guided camera-memory correlation";
                case 15: return "GPU trace saved";
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
                phaseRequestSequence = view.ReadInt32(
                    PhaseRequestSequenceOffset);
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

        private CorrelationCandidateView ReadCandidate(int index)
        {
            long offset = CandidatesOffset + index * CandidateSize;
            var candidate = new CorrelationCandidateView
            {
                Address = view.ReadUInt64(offset + 0),
                Offset = view.ReadUInt32(offset + 8),
                Source = view.ReadUInt32(offset + 12),
                Score = view.ReadSingle(offset + 16),
                ChangedMask = view.ReadUInt32(offset + 20),
                CapturedMask = view.ReadUInt32(offset + 24),
                Samples = view.ReadUInt32(offset + 28),
                Label = ReadAscii(offset + 48, 16)
            };
            for (int value = 0; value < 4; ++value)
                candidate.Values[value] =
                    view.ReadSingle(offset + 32 + value * 4);
            return candidate;
        }

        private static string PhaseName(uint phase)
        {
            switch (phase)
            {
                case 1: return "Baseline cabin";
                case 2: return "Look left";
                case 3: return "Look right";
                case 4: return "Exterior";
                case 5: return "Left mirror";
                case 6: return "Right mirror";
                case 7: return "Return cabin";
                case 8: return "Analysis complete";
                default: return "Idle";
            }
        }

        private static string PhaseMask(uint mask)
        {
            var phases = new List<string>();
            for (uint phase = 1; phase <= 7; ++phase)
                if ((mask & (1U << (int)(phase - 1))) != 0)
                    phases.Add(phase.ToString());
            return phases.Count == 0 ? "none" : string.Join(",", phases);
        }

        private static string CandidatePath(
            CorrelationCandidateView candidate)
        {
            uint source = candidate.Source & 0xFFU;
            uint parentOffset = candidate.Source >> 8;
            string root;
            switch (source)
            {
                case 1: root = "camera"; break;
                case 2: root = "camera"; break;
                case 3: root = "request"; break;
                case 4: root = "request"; break;
                case 5: root = "command"; break;
                case 6: root = "command"; break;
                default: root = "unknown"; break;
            }
            bool indirect = source == 2 || source == 4 || source == 6;
            return indirect && parentOffset != 0xFFFFFFU
                ? root + "+0x" + parentOffset.ToString("X3") +
                    " -> +0x" + candidate.Offset.ToString("X3")
                : root + "+0x" + candidate.Offset.ToString("X3");
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
                    phaseButton.Enabled = false;
                    saveButton.Enabled = false;
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
                ulong updatedTick = view.ReadUInt64(24);
                ulong runId = view.ReadUInt64(32);
                ulong frameSequence = view.ReadUInt64(40);
                uint width = view.ReadUInt32(48);
                uint height = view.ReadUInt32(52);
                uint stride = view.ReadUInt32(56);
                uint pixelBytes = view.ReadUInt32(60);
                ulong observedJobs = view.ReadUInt64(72);
                ulong probeJobs = view.ReadUInt64(80);
                ulong readbackFrames = view.ReadUInt64(96);
                ulong sampledObjects = view.ReadUInt64(104);
                uint phase = view.ReadUInt32(112);
                uint phaseMask = view.ReadUInt32(120);
                uint candidateCount = view.ReadUInt32(124);
                if (candidateCount > 16)
                    throw new InvalidOperationException(
                        "Invalid correlation candidate count");
                string stageText = ReadAscii(128, 128);
                string detail = ReadAscii(256, 512);
                string instruction = ReadAscii(768, 256);
                var candidates = new List<CorrelationCandidateView>(
                    (int)candidateCount);
                for (int index = 0; index < (int)candidateCount; ++index)
                    candidates.Add(ReadCandidate(index));
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
                    ? "Connected — legacy internal-camera path removed"
                    : "Plugin channel is stale/offline";
                connectionLabel.ForeColor = connected
                    ? Color.LightGreen
                    : Color.OrangeRed;
                runButton.Enabled = connected;
                stageLabel.Text = StageName(stage) +
                    (string.IsNullOrWhiteSpace(stageText)
                        ? ""
                        : " — " + stageText);
                stageLabel.ForeColor = stage == 11 || stage == 15
                    ? Color.LightGreen
                    : stage == 12 || stage == 13
                        ? Color.OrangeRed
                        : Color.LightSkyBlue;
                countersLabel.Text = string.Format(
                    "Run {0} | Jobs observed: {1} | Submitted control jobs: {2} | " +
                    "Trace samples: {3} | Candidates: {4} | Readback: {5}",
                    runId, observedJobs, probeJobs, sampledObjects,
                    candidateCount, readbackFrames);
                detailBox.Text = detail;
                instructionLabel.Text = string.IsNullOrWhiteSpace(instruction)
                    ? "Press Start once and wait for GPU trace saved."
                    : (phase >= 1 && phase <= 7
                        ? "Phase " + phase + "/7 — " + PhaseName(phase)
                        : PhaseName(phase)) + Environment.NewLine + instruction;
                bool correlationActive = (flags & (1U << 6)) != 0;
                if (newRun || phase != currentPhase)
                    pendingPhase = 0;
                phaseButton.Enabled = connected && correlationActive &&
                    phase >= 1 && phase <= 7 && pendingPhase != phase;
                phaseButton.Text = phase == 7
                    ? "Capture phase 7 and analyse"
                    : "Capture phase " + phase + " and continue";
                saveButton.Enabled = candidateCount > 0;

                currentPhase = phase;
                completedPhaseMask = phaseMask;
                correlationSamples = sampledObjects;
                this.observedJobs = observedJobs;
                submittedProbeJobs = probeJobs;
                currentStageText = stageText;
                currentDetail = detail;
                currentInstruction = instruction;
                if (newRun || candidates.Count != currentCandidates.Count ||
                    (candidates.Count > 0 &&
                        (currentCandidates.Count == 0 ||
                            candidates[0].Address !=
                                currentCandidates[0].Address ||
                            candidates[0].Offset !=
                                currentCandidates[0].Offset)))
                {
                    UpdateCandidates(candidates);
                }

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
                if (stage == 15 && candidateCount > 0 &&
                    runId != lastAutoSavedRun)
                {
                    SaveReport(false);
                    lastAutoSavedRun = runId;
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

        private void UpdateCandidates(
            List<CorrelationCandidateView> candidates)
        {
            currentCandidates.Clear();
            currentCandidates.AddRange(candidates);
            candidatesGrid.SuspendLayout();
            candidatesGrid.Rows.Clear();
            for (int index = 0; index < candidates.Count; ++index)
            {
                CorrelationCandidateView candidate = candidates[index];
                candidatesGrid.Rows.Add(
                    index + 1,
                    candidate.Label,
                    CandidatePath(candidate),
                    "0x" + candidate.Address.ToString("X16") +
                        " + 0x" + candidate.Offset.ToString("X3"),
                    candidate.Score.ToString("F1"),
                    PhaseMask(candidate.ChangedMask),
                    string.Format(
                        "{0:F4}, {1:F4}, {2:F4}, {3:F4}",
                        candidate.Values[0], candidate.Values[1],
                        candidate.Values[2], candidate.Values[3]));
            }
            candidatesGrid.ResumeLayout();
        }

        private void RequestPhaseCapture()
        {
            if (view == null || currentPhase < 1 || currentPhase > 7)
                return;
            try
            {
                view.Write(RequestedPhaseOffset, currentPhase);
                view.Flush();
                phaseRequestSequence = unchecked(phaseRequestSequence + 1);
                view.Write(
                    PhaseRequestSequenceOffset, phaseRequestSequence);
                view.Flush();
                pendingPhase = currentPhase;
                phaseButton.Enabled = false;
                detailBox.Text = "Phase capture requested; waiting for plugin.";
            }
            catch (Exception ex)
            {
                detailBox.Text = "Could not capture phase: " + ex.Message;
                CloseMapping();
            }
        }

        private void SaveReport(bool notify)
        {
            try
            {
                string documents = Environment.GetFolderPath(
                    Environment.SpecialFolder.MyDocuments);
                string directory = Path.Combine(documents, "ETS2");
                Directory.CreateDirectory(directory);
                string path = Path.Combine(
                    directory, "PrismCameraLabReport.txt");
                var report = new StringBuilder();
                report.AppendLine("Prism Call-Path Trace status report");
                report.AppendLine("Created: " +
                    DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
                report.AppendLine("Run: " + lastRunId);
                report.AppendLine("Stage: " + currentStageText);
                report.AppendLine("Current phase: " + currentPhase +
                    " (" + PhaseName(currentPhase) + ")");
                report.AppendLine("Completed phases: " +
                    PhaseMask(completedPhaseMask));
                report.AppendLine("Observed jobs: " + observedJobs);
                report.AppendLine("Submitted control jobs: " +
                    submittedProbeJobs);
                report.AppendLine("Bounded memory samples: " +
                    correlationSamples);
                report.AppendLine("Detail: " + currentDetail);
                report.AppendLine("Instruction: " + currentInstruction);
                report.AppendLine();
                report.AppendLine(
                    "Rank\tType\tPath\tAddress\tOffset\tScore\tChanged\tCaptured\t" +
                    "Samples\tFloat4");
                for (int index = 0;
                    index < currentCandidates.Count; ++index)
                {
                    CorrelationCandidateView candidate =
                        currentCandidates[index];
                    report.AppendLine(string.Format(
                        "{0}\t{1}\t{2}\t0x{3:X16}\t0x{4:X3}\t{5:F1}\t{6}\t{7}\t" +
                        "{8}\t{9:R},{10:R},{11:R},{12:R}",
                        index + 1, candidate.Label,
                        CandidatePath(candidate), candidate.Address,
                        candidate.Offset, candidate.Score,
                        PhaseMask(candidate.ChangedMask),
                        PhaseMask(candidate.CapturedMask), candidate.Samples,
                        candidate.Values[0], candidate.Values[1],
                        candidate.Values[2], candidate.Values[3]));
                }
                File.WriteAllText(path, report.ToString(), Encoding.UTF8);
                if (notify)
                {
                    MessageBox.Show(
                        this,
                        "Report saved to:\n" + path,
                        "Camera Lab report",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Information);
                }
                else
                {
                    detailBox.Text = currentDetail + Environment.NewLine +
                        "Report automatically saved to: " + path;
                }
            }
            catch (Exception ex)
            {
                if (notify)
                {
                    MessageBox.Show(
                        this,
                        "Could not save report: " + ex.Message,
                        "Camera Lab report",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Error);
                }
                else
                {
                    detailBox.Text = "Automatic report save failed: " +
                        ex.Message;
                }
            }
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
