using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace PrismMediaClient
{
    internal sealed class AdaptiveAudioController : IDisposable
    {
        private const uint Th32csSnapProcess = 0x00000002;
        private const int ClsctxAll = 23;

        private readonly Timer timer;
        private readonly Dictionary<string, AudioSession> sessions =
            new Dictionary<string, AudioSession>(StringComparer.Ordinal);
        private uint browserProcessId;
        private bool enabled;
        private float gain = 1.0f;
        private float pan;
        private float duckingGain = 1.0f;
        private DateTime nextDiscoveryUtc = DateTime.MinValue;
        private DateTime nextHealthCheckUtc = DateTime.MinValue;
        private bool applyPending = true;

        internal AdaptiveAudioController()
        {
            timer = new Timer { Interval = 100 };
            timer.Tick += (_, __) => Tick();
            timer.Start();
        }

        internal void SetBrowserProcessId(uint processId)
        {
            browserProcessId = processId;
            nextDiscoveryUtc = DateTime.MinValue;
            applyPending = true;
        }

        internal void SetDesired(bool spatialEnabled, float desiredGain, float desiredPan)
        {
            float nextGain = Math.Max(0.0f, Math.Min(1.0f, desiredGain));
            float nextPan = Math.Max(-1.0f, Math.Min(1.0f, desiredPan));
            if (enabled == spatialEnabled &&
                Math.Abs(gain - nextGain) < 0.0005f &&
                Math.Abs(pan - nextPan) < 0.0005f)
                return;

            enabled = spatialEnabled;
            gain = nextGain;
            pan = nextPan;
            applyPending = true;
        }

        internal void SetDucking(float desiredGain)
        {
            float nextGain = Math.Max(0.0f, Math.Min(1.0f, desiredGain));
            if (Math.Abs(duckingGain - nextGain) < 0.0005f)
                return;

            duckingGain = nextGain;
            applyPending = true;
        }

        internal void RequestSessionRefresh(string reason)
        {
            nextDiscoveryUtc = DateTime.MinValue;
            applyPending = true;
            ClientDiagnosticLog.Write(
                "audio", "Audio-session refresh requested (" + reason +
                "). Current spatial gain=" + gain.ToString("0.000") +
                ", environment gain=" + duckingGain.ToString("0.000") +
                ", pan=" + pan.ToString("0.000") + ".");
        }

        private void Tick()
        {
            try
            {
                DateTime now = DateTime.UtcNow;
                bool sessionAdded = false;
                if (browserProcessId != 0 && now >= nextDiscoveryUtc)
                {
                    sessionAdded = DiscoverSessions();
                    nextDiscoveryUtc = now.AddSeconds(1);
                }

                bool healthCheck = now >= nextHealthCheckUtc;
                if (!applyPending && !sessionAdded && !healthCheck)
                    return;

                float combinedGain = gain * duckingGain;
                bool processing = enabled || duckingGain < 0.999f;
                float combinedPan = enabled ? pan : 0.0f;
                List<string> expired = null;
                foreach (KeyValuePair<string, AudioSession> item in sessions)
                {
                    try
                    {
                        item.Value.Apply(
                            processing, combinedGain, combinedPan);
                    }
                    catch
                    {
                        item.Value.Dispose();
                        if (expired == null)
                            expired = new List<string>();
                        expired.Add(item.Key);
                    }
                }
                if (expired != null)
                {
                    foreach (string key in expired)
                        sessions.Remove(key);
                    nextDiscoveryUtc = DateTime.MinValue;
                }
                applyPending = false;
                nextHealthCheckUtc = now.AddSeconds(5);
            }
            catch
            {
                // Audio devices and WebView child processes can be recreated at
                // any time. A later discovery tick retries without interrupting
                // video playback.
                nextDiscoveryUtc = DateTime.MinValue;
            }
        }

        private bool DiscoverSessions()
        {
            HashSet<uint> targetProcesses = BuildProcessTree(browserProcessId);
            if (targetProcesses.Count == 0)
                return false;

            bool sessionAdded = false;

            IMMDeviceEnumerator enumerator = null;
            IMMDevice device = null;
            IAudioSessionManager2 manager = null;
            IAudioSessionEnumerator sessionEnumerator = null;
            try
            {
                // A COM coclass is not statically convertible to its interface
                // with every C# compiler used by the GitHub Windows runners.
                // Cast through object so the runtime performs QueryInterface.
                enumerator =
                    (IMMDeviceEnumerator)(object)new MMDeviceEnumerator();
                Marshal.ThrowExceptionForHR(enumerator.GetDefaultAudioEndpoint(
                    EDataFlow.Render, ERole.Multimedia, out device));

                Guid managerId = typeof(IAudioSessionManager2).GUID;
                object managerObject;
                Marshal.ThrowExceptionForHR(device.Activate(
                    ref managerId, ClsctxAll, IntPtr.Zero, out managerObject));
                manager = (IAudioSessionManager2)managerObject;
                Marshal.ThrowExceptionForHR(manager.GetSessionEnumerator(
                    out sessionEnumerator));
                Marshal.ThrowExceptionForHR(sessionEnumerator.GetCount(
                    out int count));

                for (int index = 0; index < count; ++index)
                {
                    IAudioSessionControl2 control = null;
                    try
                    {
                        Marshal.ThrowExceptionForHR(
                            sessionEnumerator.GetSession(index, out control));
                        Marshal.ThrowExceptionForHR(control.GetProcessId(
                            out uint processId));
                        if (!targetProcesses.Contains(processId))
                            continue;

                        string instanceId;
                        if (control.GetSessionInstanceIdentifier(
                            out instanceId) < 0 ||
                            string.IsNullOrEmpty(instanceId))
                            instanceId = "pid:" + processId;

                        if (sessions.ContainsKey(instanceId))
                            continue;

                        sessions.Add(instanceId, new AudioSession(control));
                        sessionAdded = true;
                        control = null; // AudioSession now owns the RCW.
                    }
                    catch
                    {
                        // An audio session can expire while it is enumerated.
                    }
                    finally
                    {
                        ReleaseCom(control);
                    }
                }
            }
            finally
            {
                ReleaseCom(sessionEnumerator);
                ReleaseCom(manager);
                ReleaseCom(device);
                ReleaseCom(enumerator);
            }
            if (sessionAdded)
            {
                ClientDiagnosticLog.Write(
                    "audio", "Attached adaptive processing to " +
                    sessions.Count + " WebView audio session(s).");
            }
            return sessionAdded;
        }

        private static HashSet<uint> BuildProcessTree(uint rootProcessId)
        {
            var parents = new Dictionary<uint, uint>();
            IntPtr snapshot = CreateToolhelp32Snapshot(
                Th32csSnapProcess, 0);
            if (snapshot == new IntPtr(-1))
                return new HashSet<uint>();

            try
            {
                var entry = new ProcessEntry32();
                entry.Size = (uint)Marshal.SizeOf(typeof(ProcessEntry32));
                if (Process32First(snapshot, ref entry))
                {
                    do
                    {
                        parents[entry.ProcessId] = entry.ParentProcessId;
                        entry.Size = (uint)Marshal.SizeOf(typeof(ProcessEntry32));
                    }
                    while (Process32Next(snapshot, ref entry));
                }
            }
            finally
            {
                CloseHandle(snapshot);
            }

            var result = new HashSet<uint> { rootProcessId };
            bool changed;
            do
            {
                changed = false;
                foreach (KeyValuePair<uint, uint> process in parents)
                {
                    if (!result.Contains(process.Key) &&
                        result.Contains(process.Value))
                    {
                        result.Add(process.Key);
                        changed = true;
                    }
                }
            }
            while (changed);
            return result;
        }

        public void Dispose()
        {
            timer.Stop();
            foreach (AudioSession session in sessions.Values)
            {
                session.Apply(false, 1.0f, 0.0f);
                session.Dispose();
            }
            sessions.Clear();
            timer.Dispose();
        }

        private sealed class AudioSession : IDisposable
        {
            private IAudioSessionControl2 control;
            private IChannelAudioVolume channels;
            private ISimpleAudioVolume simple;
            private float[] originalChannels;
            private float originalMaster = 1.0f;

            internal AudioSession(IAudioSessionControl2 sessionControl)
            {
                control = sessionControl;
                try
                {
                    channels = (IChannelAudioVolume)sessionControl;
                    Marshal.ThrowExceptionForHR(channels.GetChannelCount(
                        out uint count));
                    originalChannels = new float[checked((int)count)];
                    for (uint index = 0; index < count; ++index)
                    {
                        Marshal.ThrowExceptionForHR(channels.GetChannelVolume(
                            index, out originalChannels[(int)index]));
                    }
                }
                catch
                {
                    channels = null;
                    originalChannels = null;
                    try
                    {
                        simple = (ISimpleAudioVolume)sessionControl;
                        Marshal.ThrowExceptionForHR(
                            simple.GetMasterVolume(out originalMaster));
                    }
                    catch
                    {
                        simple = null;
                    }
                }
            }

            internal void Apply(bool spatialEnabled, float gain, float pan)
            {
                if (!spatialEnabled)
                {
                    gain = 1.0f;
                    pan = 0.0f;
                }

                if (channels != null && originalChannels != null)
                {
                    float angle = (pan + 1.0f) *
                        (float)Math.PI * 0.25f;
                    float left = Math.Min(
                        1.0f, (float)Math.Cos(angle) * 1.41421356f);
                    float right = Math.Min(
                        1.0f, (float)Math.Sin(angle) * 1.41421356f);

                    Guid eventContext = Guid.Empty;
                    for (int index = 0;
                        index < originalChannels.Length; ++index)
                    {
                        float channelGain = gain;
                        if (index == 0)
                            channelGain *= left;
                        else if (index == 1)
                            channelGain *= right;
                        channels.SetChannelVolume(
                            (uint)index,
                            Math.Max(0.0f, Math.Min(
                                1.0f,
                                originalChannels[index] * channelGain)),
                            ref eventContext);
                    }
                }
                else if (simple != null)
                {
                    Guid eventContext = Guid.Empty;
                    simple.SetMasterVolume(
                        Math.Max(0.0f, Math.Min(
                            1.0f, originalMaster * gain)),
                        ref eventContext);
                }
            }

            public void Dispose()
            {
                ReleaseCom(channels);
                ReleaseCom(simple);
                ReleaseCom(control);
                channels = null;
                simple = null;
                control = null;
            }
        }

        private static void ReleaseCom(object value)
        {
            if (value != null && Marshal.IsComObject(value))
            {
                try { Marshal.ReleaseComObject(value); }
                catch { }
            }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateToolhelp32Snapshot(
            uint flags, uint processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto,
            SetLastError = true)]
        private static extern bool Process32First(
            IntPtr snapshot, ref ProcessEntry32 entry);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto,
            SetLastError = true)]
        private static extern bool Process32Next(
            IntPtr snapshot, ref ProcessEntry32 entry);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct ProcessEntry32
        {
            public uint Size;
            public uint Usage;
            public uint ProcessId;
            public IntPtr DefaultHeapId;
            public uint ModuleId;
            public uint Threads;
            public uint ParentProcessId;
            public int BasePriority;
            public uint Flags;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string ExeFile;
        }

        private enum EDataFlow
        {
            Render,
            Capture,
            All
        }

        private enum ERole
        {
            Console,
            Multimedia,
            Communications
        }

        [ComImport]
        [Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
        private sealed class MMDeviceEnumerator
        {
        }

        [ComImport]
        [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDeviceEnumerator
        {
            [PreserveSig]
            int EnumAudioEndpoints(
                EDataFlow dataFlow, uint stateMask, out IntPtr devices);

            [PreserveSig]
            int GetDefaultAudioEndpoint(
                EDataFlow dataFlow, ERole role, out IMMDevice endpoint);

            [PreserveSig]
            int GetDevice(
                [MarshalAs(UnmanagedType.LPWStr)] string id,
                out IMMDevice device);

            [PreserveSig]
            int RegisterEndpointNotificationCallback(IntPtr client);

            [PreserveSig]
            int UnregisterEndpointNotificationCallback(IntPtr client);
        }

        [ComImport]
        [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMMDevice
        {
            [PreserveSig]
            int Activate(
                ref Guid interfaceId,
                int classContext,
                IntPtr activationParameters,
                [MarshalAs(UnmanagedType.IUnknown)] out object instance);

            [PreserveSig]
            int OpenPropertyStore(int access, out IntPtr properties);

            [PreserveSig]
            int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);

            [PreserveSig]
            int GetState(out uint state);
        }

        [ComImport]
        [Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionManager2
        {
            [PreserveSig]
            int GetAudioSessionControl(
                ref Guid sessionGuid, uint streamFlags,
                out IAudioSessionControl2 sessionControl);

            [PreserveSig]
            int GetSimpleAudioVolume(
                ref Guid sessionGuid, uint streamFlags,
                out ISimpleAudioVolume simpleVolume);

            [PreserveSig]
            int GetSessionEnumerator(
                out IAudioSessionEnumerator sessionEnumerator);
        }

        [ComImport]
        [Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionEnumerator
        {
            [PreserveSig]
            int GetCount(out int sessionCount);

            [PreserveSig]
            int GetSession(
                int sessionIndex,
                out IAudioSessionControl2 sessionControl);
        }

        [ComImport]
        [Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IAudioSessionControl2
        {
            [PreserveSig]
            int GetState(out int state);

            [PreserveSig]
            int GetDisplayName(
                [MarshalAs(UnmanagedType.LPWStr)] out string displayName);

            [PreserveSig]
            int SetDisplayName(
                [MarshalAs(UnmanagedType.LPWStr)] string displayName,
                ref Guid eventContext);

            [PreserveSig]
            int GetIconPath(
                [MarshalAs(UnmanagedType.LPWStr)] out string iconPath);

            [PreserveSig]
            int SetIconPath(
                [MarshalAs(UnmanagedType.LPWStr)] string iconPath,
                ref Guid eventContext);

            [PreserveSig]
            int GetGroupingParam(out Guid groupingId);

            [PreserveSig]
            int SetGroupingParam(
                ref Guid groupingId, ref Guid eventContext);

            [PreserveSig]
            int RegisterAudioSessionNotification(IntPtr client);

            [PreserveSig]
            int UnregisterAudioSessionNotification(IntPtr client);

            [PreserveSig]
            int GetSessionIdentifier(
                [MarshalAs(UnmanagedType.LPWStr)] out string identifier);

            [PreserveSig]
            int GetSessionInstanceIdentifier(
                [MarshalAs(UnmanagedType.LPWStr)] out string identifier);

            [PreserveSig]
            int GetProcessId(out uint processId);

            [PreserveSig]
            int IsSystemSoundsSession();

            [PreserveSig]
            int SetDuckingPreference(
                [MarshalAs(UnmanagedType.Bool)] bool optOut);
        }

        [ComImport]
        [Guid("1C158861-B533-4B30-B1CF-E853E51C59B8")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IChannelAudioVolume
        {
            [PreserveSig]
            int GetChannelCount(out uint channelCount);

            [PreserveSig]
            int SetChannelVolume(
                uint channelIndex, float level, ref Guid eventContext);

            [PreserveSig]
            int GetChannelVolume(uint channelIndex, out float level);

            [PreserveSig]
            int SetAllVolumes(
                uint channelCount, float[] levels, ref Guid eventContext);

            [PreserveSig]
            int GetAllVolumes(uint channelCount, float[] levels);
        }

        [ComImport]
        [Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface ISimpleAudioVolume
        {
            [PreserveSig]
            int SetMasterVolume(float level, ref Guid eventContext);

            [PreserveSig]
            int GetMasterVolume(out float level);

            [PreserveSig]
            int SetMute(
                [MarshalAs(UnmanagedType.Bool)] bool muted,
                ref Guid eventContext);

            [PreserveSig]
            int GetMute([MarshalAs(UnmanagedType.Bool)] out bool muted);
        }
    }
}
