(() => {
    if (window.__prismLowPassState)
        return;

    const state = {
        enabled: false,
        cutoff: 20000,
        context: null,
        nodes: [],
        attached: new WeakSet(),
        attaching: false
    };
    window.__prismLowPassState = state;

    function updateNodes() {
        for (const node of state.nodes) {
            node.type = state.enabled ? "lowpass" : "allpass";
            node.Q.value = 0.707;
            node.frequency.setTargetAtTime(
                state.enabled ? state.cutoff : 20000,
                state.context.currentTime,
                0.035);
        }
    }

    function isSafeToRoute(media) {
        if (media.crossOrigin)
            return true;
        const source = media.currentSrc || media.src || "";
        if (!source)
            return false;
        if (source.startsWith("blob:") || source.startsWith("data:"))
            return true;
        try {
            return new URL(source, location.href).origin === location.origin;
        } catch (_) {
            return false;
        }
    }

    async function attachMedia() {
        if (!state.enabled || state.attaching)
            return;
        state.attaching = true;
        try {
            if (!state.context) {
                const AudioContextType =
                    window.AudioContext || window.webkitAudioContext;
                if (!AudioContextType)
                    return;
                state.context = new AudioContextType();
            }

            if (state.context.state !== "running") {
                try {
                    await state.context.resume();
                } catch (_) {
                    return;
                }
            }

            // Never reroute audio into a suspended graph. This is the
            // fail-safe for autoplay-restricted pages.
            if (state.context.state !== "running")
                return;

            for (const media of document.querySelectorAll("video,audio")) {
                if (state.attached.has(media) || !isSafeToRoute(media))
                    continue;
                try {
                    const source =
                        state.context.createMediaElementSource(media);
                    const filter =
                        state.context.createBiquadFilter();
                    source.connect(filter);
                    filter.connect(state.context.destination);
                    state.nodes.push(filter);
                    state.attached.add(media);
                } catch (_) {
                    // A page can already own a MediaElementSource. Leave
                    // that element untouched and keep its original audio.
                }
            }
            updateNodes();
        } finally {
            state.attaching = false;
        }
    }

    window.prismSetLowPass = (cutoff, enabled) => {
        state.cutoff = Math.max(20, Math.min(20000, Number(cutoff) || 20000));
        state.enabled = Boolean(enabled) && state.cutoff < 19500;
        updateNodes();
        if (state.enabled)
            void attachMedia();
    };

    new MutationObserver(() => {
        if (state.enabled)
            void attachMedia();
    }).observe(document.documentElement, { childList: true, subtree: true });

    addEventListener("pointerdown", () => void attachMedia(), true);
    addEventListener("keydown", () => void attachMedia(), true);
    setInterval(() => {
        if (state.enabled)
            void attachMedia();
    }, 1000);
})();
