# 3.10.1 audio reliability fix

Changes applied to the supplied 3.10.1 source:

- Prevents unsafe cross-origin `createMediaElementSource()` routing that can make Chromium/WebView2 output silence when CORS permission is absent. Cross-origin media now stays on the page-owned Web Audio path.
- Starts WebView2 with `--autoplay-policy=no-user-gesture-required` so the silent helper does not depend on an activation click before unmuted playback.
- Explicitly unmutes YouTube on player readiness unless the user intentionally used the plugin Mute command. Direct media follows the same tracked mute state.
- Reworks Windows WebView audio-session processing so master gain/ducking uses `ISimpleAudioVolume` while per-channel controls are reserved for stereo pan. The previous implementation ignored master volume whenever channel volume was available.
- Logs each discovered WebView audio session's baseline master/mute/channel state for diagnosis.
- Explicitly initializes Native Direct Media to unmuted, 100% volume.

The adaptive low-pass still works, but direct media-element routing is now restricted to same-origin/inline media to avoid CORS-induced silence.
