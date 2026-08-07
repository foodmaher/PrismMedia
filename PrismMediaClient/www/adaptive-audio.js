(() => {
    if (window.__prismLowPassState)
        return;

    const AudioContextType = window.AudioContext || window.webkitAudioContext;
    const audioNodePrototype = window.AudioNode && window.AudioNode.prototype;
    const nativeConnect = audioNodePrototype && audioNodePrototype.connect;
    const nativeDisconnect = audioNodePrototype && audioNodePrototype.disconnect;
    const mediaPrototype = window.HTMLMediaElement &&
        window.HTMLMediaElement.prototype;
    const nativeMediaPlay = mediaPrototype && mediaPrototype.play;
    const nativeMediaLoad = mediaPrototype && mediaPrototype.load;
    const state = {
        enabled: false,
        cutoff: 20000,
        directContext: null,
        nodes: [],
        attached: new WeakSet(),
        observedMedia: new Set(),
        observedReported: new WeakSet(),
        playbackTracked: new WeakSet(),
        unsafeReported: new WeakSet(),
        failedReported: new WeakSet(),
        destinationGraphs: new WeakMap(),
        routedOutputs: new WeakMap(),
        internalNodes: new WeakSet(),
        attaching: false,
        lastStateReport: ""
    };
    window.__prismLowPassState = state;

    function report(message) {
        try {
            if (window.chrome && window.chrome.webview)
                window.chrome.webview.postMessage("log|audio|" + message);
        } catch (_) {
            // Diagnostics must never affect playback.
        }
    }

    function rememberMedia(media, reason) {
        if (!media)
            return;
        state.observedMedia.add(media);
        if (!state.playbackTracked.has(media)) {
            state.playbackTracked.add(media);
            const reportPlaybackState = () => {
                try {
                    if (window.chrome && window.chrome.webview) {
                        window.chrome.webview.postMessage(
                            "log|spotify|Observed media state: " +
                            (!media.paused && !media.ended
                                ? "playing" : "paused") + ".");
                    }
                } catch (_) {
                    // Playback reporting must not affect the media element.
                }
            };
            media.addEventListener("play", reportPlaybackState);
            media.addEventListener("pause", reportPlaybackState);
            media.addEventListener("ended", reportPlaybackState);
        }
        if (!state.observedReported.has(media)) {
            state.observedReported.add(media);
            report("Observed a media element through " + reason +
                "; detached Spotify audio is now trackable.");
        }
        if (state.enabled)
            void attachMedia();
    }

    // Spotify commonly plays through an Audio element kept outside the DOM.
    // A document.querySelectorAll scan can never see it, so remember elements
    // at the browser playback boundary. The native method and its Promise are
    // returned unchanged to avoid altering Spotify's autoplay behavior.
    if (nativeMediaPlay) {
        mediaPrototype.play = function() {
            rememberMedia(this, "HTMLMediaElement.play()");
            return nativeMediaPlay.apply(this, arguments);
        };
    }
    if (nativeMediaLoad) {
        mediaPrototype.load = function() {
            rememberMedia(this, "HTMLMediaElement.load()");
            return nativeMediaLoad.apply(this, arguments);
        };
    }

    window.prismGetPlaybackState = () => {
        let observed = false;
        for (const media of state.observedMedia) {
            observed = true;
            if (!media.paused && !media.ended)
                return "playing";
        }
        return observed ? "paused" : "unknown";
    };

    window.prismAdjustObservedVolume = delta => {
        let changed = false;
        const amount = Number(delta) || 0;
        for (const media of state.observedMedia) {
            try {
                media.volume = Math.max(
                    0, Math.min(1, Number(media.volume) + amount));
                changed = true;
            } catch (_) {
                // Protected elements can reject direct volume changes.
            }
        }
        return changed;
    };

    function configureFilter(filter, context) {
        filter.type = state.enabled ? "lowpass" : "allpass";
        filter.Q.value = 0.707;
        filter.frequency.setTargetAtTime(
            state.enabled ? state.cutoff : 20000,
            context.currentTime,
            0.035);
    }

    function registerFilter(filter, context, route) {
        state.nodes.push({ filter, context, route });
        configureFilter(filter, context);
    }

    function updateNodes() {
        for (const node of state.nodes) {
            try {
                configureFilter(node.filter, node.context);
            } catch (_) {
                // A navigation can close an AudioContext asynchronously.
            }
        }
    }

    function ensureDestinationGraph(context) {
        let graph = state.destinationGraphs.get(context);
        if (graph)
            return graph;

        const input = context.createGain();
        const filter = context.createBiquadFilter();
        state.internalNodes.add(input);
        state.internalNodes.add(filter);

        // Use Chromium's unmodified connect function so these two internal
        // connections cannot be intercepted recursively.
        nativeConnect.call(input, filter);
        nativeConnect.call(filter, context.destination);
        graph = { input, filter };
        state.destinationGraphs.set(context, graph);
        registerFilter(filter, context, "page-output");
        report("Attached low-pass to a page-owned Web Audio output graph.");
        return graph;
    }

    // Spotify can create and own its MediaElementSource before our fallback
    // scanner sees the audio element. Intercept only final connections to the
    // page's AudioDestinationNode and place one shared filter at that output.
    // YouTube/direct HTML media continues to use attachMedia() below.
    if (nativeConnect) {
        audioNodePrototype.connect = function(destination) {
            if (destination && this.context &&
                destination === this.context.destination &&
                !state.internalNodes.has(this)) {
                try {
                    const graph = ensureDestinationGraph(this.context);
                    if (arguments.length >= 2)
                        nativeConnect.call(this, graph.input, arguments[1]);
                    else
                        nativeConnect.call(this, graph.input);
                    state.routedOutputs.set(this, graph.input);
                    return destination;
                } catch (error) {
                    report("Page-output interception failed; using original route (" +
                        String(error && error.name || "unknown") + ").");
                }
            }
            return nativeConnect.apply(this, arguments);
        };
        report("Web Audio destination hook installed.");
    }

    // Preserve page disconnect(destination, ...) behavior after replacing its
    // destination with the shared Prism input node.
    if (nativeDisconnect) {
        audioNodePrototype.disconnect = function(destination) {
            const routed = state.routedOutputs.get(this);
            if (routed && destination === this.context.destination) {
                const args = Array.prototype.slice.call(arguments);
                args[0] = routed;
                return nativeDisconnect.apply(this, args);
            }
            return nativeDisconnect.apply(this, arguments);
        };
    }

    function isSafeToRoute(media) {
        const source = media.currentSrc || media.src || "";
        if (!source)
            return false;

        // A crossOrigin attribute alone does not prove that the response is
        // CORS-clean. Once createMediaElementSource() takes ownership,
        // Chromium intentionally outputs silence for a tainted cross-origin
        // resource. Only route same-origin/inline media, plus the app-owned
        // traffic.prism.local virtual host whose resources are explicitly
        // exposed by WebView2 with HostResourceAccessKind.Allow.
        if (source.startsWith("data:"))
            return true;
        try {
            const url = new URL(source, location.href);
            return url.origin === location.origin ||
                url.hostname === "traffic.prism.local";
        } catch (_) {
            return false;
        }
    }

    async function attachMedia() {
        if (!state.enabled || state.attaching || !AudioContextType)
            return;
        state.attaching = true;
        try {
            if (!state.directContext)
                state.directContext = new AudioContextType();

            if (state.directContext.state !== "running") {
                try {
                    await state.directContext.resume();
                } catch (_) {
                    return;
                }
            }

            // Never reroute audio into a suspended graph. This is the
            // fail-safe for autoplay-restricted pages.
            if (state.directContext.state !== "running")
                return;

            const candidates = new Set(state.observedMedia);
            for (const media of document.querySelectorAll("video,audio"))
                candidates.add(media);

            for (const media of candidates) {
                if (state.attached.has(media))
                    continue;
                if (!isSafeToRoute(media)) {
                    if (!state.unsafeReported.has(media)) {
                        state.unsafeReported.add(media);
                        report("Media-element fallback skipped an unsafe " +
                            "cross-origin source; waiting for page-output route.");
                    }
                    continue;
                }
                try {
                    const source =
                        state.directContext.createMediaElementSource(media);
                    const filter =
                        state.directContext.createBiquadFilter();
                    state.internalNodes.add(filter);
                    nativeConnect.call(source, filter);
                    nativeConnect.call(filter, state.directContext.destination);
                    registerFilter(filter, state.directContext, "media-element");
                    state.attached.add(media);
                    report("Attached low-pass directly to a media element; " +
                        "attached routes=" + state.nodes.length + ".");
                } catch (error) {
                    // The page can already own this MediaElementSource. Its
                    // output is handled by the destination hook above.
                    if (!state.failedReported.has(media)) {
                        state.failedReported.add(media);
                        report("Direct media attachment unavailable (" +
                            String(error && error.name || "unknown") +
                            "); waiting for page-output route.");
                    }
                }
            }
            updateNodes();
        } finally {
            state.attaching = false;
        }
    }

    window.prismSetLowPass = (cutoff, enabled) => {
        state.cutoff = Math.max(
            20, Math.min(20000, Number(cutoff) || 20000));
        state.enabled = Boolean(enabled) && state.cutoff < 19500;
        updateNodes();

        const reportKey = (state.enabled ? "on:" : "off:") +
            Math.round(state.cutoff) + ":" + state.nodes.length;
        if (reportKey !== state.lastStateReport) {
            state.lastStateReport = reportKey;
            report("Low-pass state: " +
                (state.enabled ? "enabled" : "disabled") +
                ", cutoff=" + Math.round(state.cutoff) +
                " Hz, attached routes=" + state.nodes.length + ".");
        }

        if (state.enabled)
            void attachMedia();
    };

    const mediaObserver = new MutationObserver(() => {
        if (state.enabled)
            void attachMedia();
    });
    const observeMediaElements = () => {
        if (document.documentElement) {
            mediaObserver.observe(
                document.documentElement,
                { childList: true, subtree: true });
        }
    };
    if (document.documentElement)
        observeMediaElements();
    else
        addEventListener("DOMContentLoaded", observeMediaElements, { once: true });

    addEventListener("pointerdown", () => void attachMedia(), true);
    addEventListener("keydown", () => void attachMedia(), true);
    setInterval(() => {
        if (state.enabled)
            void attachMedia();
    }, 1000);
})();
