# PrismMedia 4.0.0 — selective addon isolation diagnostic

The November 6 capture with the corrected Tjs path proved that
`/vehicle/truck/upgrade/bagps/tablet_gps.tobj` was redirected and consumed.
After the truck reload, the validated call at RVA `0x543845` produced two
persistent addon pairs (256 and 255 samples) plus one transient row.

It also disproved two assumptions from the static analysis:

- The return value was the same RCX value for every captured addon; it was not
  a unique per-display texture handle.
- The value at descriptor offset `+0x160` was float-like for both persistent
  pairs, so it is not treated as a stable addon index.

This revision therefore does not hard-code either value. It learns persistent
live `(RDX,R8)` pairs from `addon arm`, exposes them as numbered candidates,
and can suppress one candidate for a bounded window while every unmatched
addon continues through the original game function.

## One-build test

Keep the same truck and accessories loaded throughout:

```text
capabilities
addon arm 60
addon candidates
addon isolate 0 20
addon isolate status
addon isolate 1 20
addon isolate status
addon isolate off
```

`capabilities` must include `addonIsolation=1`. During each 20-second window,
observe the Tjs custom media and smartphone GPS separately. The command forces
the old global fallback and per-draw router off only for the bounded test, then
restores their previous modes automatically.

Candidate pointers are session-local. If the truck, accessories, or textures
reload after capture, run `addon arm 60` again before isolation.

This remains a diagnostic, not a claimed final fix. Success means one candidate
preserves custom media while the smartphone stays native. A crash or no visual
change means the function cannot safely be suppressed at this boundary; the
next investigation must move to a later verified texture-use point.
