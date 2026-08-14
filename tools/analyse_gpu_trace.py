#!/usr/bin/env python3
"""Summarise PrismIndependentGpuTrace.bin without loading game binaries."""

from __future__ import annotations

import argparse
import collections
import pathlib
import struct


HEADER = struct.Struct("<IIIIQQQQ")
EVENT = struct.Struct("<QIIQQQQQQ")
NAMES = {
    1: "trace_begin",
    2: "trace_end",
    3: "job_enter",
    4: "job_exit",
    5: "probe_job_enter",
    6: "probe_job_exit",
    7: "om_render_targets",
    8: "om_render_targets_uav",
    9: "vs_constant_buffers",
    10: "rs_viewports",
    11: "draw",
    12: "draw_indexed",
    13: "draw_instanced",
    14: "draw_indexed_instanced",
    15: "update_subresource",
    16: "copy_region",
    17: "copy_resource",
    18: "resolve",
    19: "execute_command_list",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    args = parser.parse_args()
    data = args.trace.read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit("Trace is shorter than its header")
    magic, version, header_size, event_size, frequency, start_qpc, count, dropped = (
        HEADER.unpack_from(data)
    )
    if magic != 0x54554750 or version != 1:
        raise SystemExit("Unsupported trace format")
    if header_size != HEADER.size or event_size != EVENT.size:
        raise SystemExit("Trace record size mismatch")
    available = (len(data) - header_size) // event_size
    count = min(count, available)
    events = [EVENT.unpack_from(data, header_size + index * event_size)
              for index in range(count)]
    totals = collections.Counter(event[2] for event in events)
    print(f"events={count} dropped={dropped} qpc_frequency={frequency}")
    for event_type in sorted(totals):
        print(f"{NAMES.get(event_type, f'unknown_{event_type}')}={totals[event_type]}")

    probes = [event for event in events if event[2] == 5]
    if not probes:
        print("result=no submitted probe job entered Prism3D")
        return 2
    probe = probes[0]
    probe_qpc = probe[0]
    window_ticks = max(1, frequency // 4)  # 250 ms after entry
    window = [event for event in events
              if probe_qpc <= event[0] <= probe_qpc + window_ticks]
    baseline = [event for event in events
                if probe_qpc - window_ticks <= event[0] < probe_qpc]
    window_totals = collections.Counter(event[2] for event in window)
    baseline_totals = collections.Counter(event[2] for event in baseline)
    draw_total = sum(window_totals[k] for k in (11, 12, 13, 14))
    baseline_draws = sum(baseline_totals[k] for k in (11, 12, 13, 14))
    print("probe_window_ms=250")
    print(f"probe_window_events={len(window)}")
    print(f"baseline_window_events={len(baseline)}")
    print(f"probe_window_draws={draw_total}")
    print(f"baseline_window_draws={baseline_draws}")
    print(f"probe_window_target_binds={window_totals[7] + window_totals[8]}")
    print("baseline_window_target_binds="
          f"{baseline_totals[7] + baseline_totals[8]}")
    print(f"probe_window_constant_buffer_binds={window_totals[9]}")
    print(f"probe_window_command_lists={window_totals[19]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
