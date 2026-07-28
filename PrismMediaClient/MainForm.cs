using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PrismMediaClient
{
    internal sealed class MainForm : Form
    {
        private const int WmCopyData = 0x004A;
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
        private bool ready;

        internal MainForm(string initialUrl)
        {
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
            Shown += async (_, __) => await InitializePlayerAsync();
        }

        private async Task InitializePlayerAsync()
        {
            try
            {
                await webView.EnsureCoreWebView2Async();
                webView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
                webView.CoreWebView2.Settings.AreDevToolsEnabled = false;
                webView.CoreWebView2.Settings.IsStatusBarEnabled = false;
                webView.CoreWebView2.Settings.IsZoomControlEnabled = false;

                string contentFolder = Path.Combine(
                    AppDomain.CurrentDomain.BaseDirectory, "www");
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
            }
            catch (Exception error)
            {
                Text = "Prism Media Client - WebView2 error";
                MessageBox.Show(
                    "The Prism Media Client could not start WebView2.\n\n" +
                    error.Message +
                    "\n\nInstall the Microsoft Edge WebView2 Runtime and try again.",
                    "Prism Media Client", MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
            }
        }

        protected override void WndProc(ref Message message)
        {
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
                        if (ready)
                            await ApplyCommandAsync(command);
                        else
                            pendingCommands.Enqueue(command);
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
    }
}
