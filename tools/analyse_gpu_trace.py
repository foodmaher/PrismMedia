#!/usr/bin/env python3
"""Summarise PrismIndependentGpuTrace.bin without game binaries."""

from __future__ import annotations

import argparse
import collections
import pathlib
import struct


HEADER = struct.Struct("<IIIIQQQQ")
EVENT = struct.Struct("<QIIQQQQQQ")
NAMES = {
    1: "trace_begin", 2: "trace_end", 3: "job_enter", 4: "job_exit",
    5: "probe_job_enter", 6: "probe_job_exit", 7: "om_render_targets",
    8: "om_render_targets_uav", 9: "vs_constant_buffers",
    10: "rs_viewports", 11: "draw", 12: "draw_indexed",
    13: "draw_instanced", 14: "draw_indexed_instanced",
    15: "update_subresource", 16: "copy_region", 17: "copy_resource",
    18: "resolve", 19: "execute_command_list", 20: "worker_enter",
    21: "worker_exit", 22: "scheduler_enter", 23: "scheduler_exit",
    24: "submit_enter", 25: "submit_exit", 26: "frame_marker",
    27: "object_digest", 28: "stack_frame", 29: "draw_batch",
    30: "gpu_thread_selected",
}
ROLES = {
    0: "unknown", 1: "real_control_dispatch",
    2: "probe_nested_dispatch", 3: "probe_confirmed_dispatch",
    4: "mirror_worker", 5: "scheduler", 6: "plugin_submit",
    7: "engine_submit",
}


def signed32(value: int) -> int:
    return value if value < 0x80000000 else value - 0x100000000


def draw_count(events: list[tuple[int, ...]]) -> int:
    direct = sum(1 for event in events if event[2] in (11, 12, 13, 14))
    batched = sum(event[4] for event in events if event[2] == 29)
    return direct + batched


def pair_durations(events, enter_type, exit_type, frequency):
    stacks: dict[int, list[tuple[int, ...]]] = collections.defaultdict(list)
    result = []
    for event in events:
        if event[2] == enter_type:
            stacks[event[1]].append(event)
        elif event[2] == exit_type and stacks[event[1]]:
            start = stacks[event[1]].pop()
            result.append((event[0] - start[0]) * 1000.0 / frequency)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    args = parser.parse_args()
    data = args.trace.read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit("Trace is shorter than its header")
    magic, version, header_size, event_size, frequency, _, count, dropped = (
        HEADER.unpack_from(data)
    )
    if magic != 0x54554750 or version not in (1, 2):
        raise SystemExit("Unsupported trace format")
    if header_size != HEADER.size or event_size != EVENT.size:
        raise SystemExit("Trace record size mismatch")
    available = (len(data) - header_size) // event_size
    count = min(count, available)
    events = [EVENT.unpack_from(data, header_size + index * event_size)
              for index in range(count)]
    totals = collections.Counter(event[2] for event in events)
    print(f"format={version} events={count} dropped={dropped} "
          f"qpc_frequency={frequency}")
    for event_type in sorted(totals):
        print(f"{NAMES.get(event_type, f'unknown_{event_type}')}="
              f"{totals[event_type]}")
    print(f"represented_draws={draw_count(events)}")

    probes = [event for event in events if event[2] == 5]
    if not probes:
        print("result=no submitted probe job entered Prism3D")
        return 2

    if version == 1:
        probe_qpc = probes[0][0]
        ticks = max(1, frequency // 4)
        window = [event for event in events
                  if probe_qpc <= event[0] <= probe_qpc + ticks]
        baseline = [event for event in events
                    if probe_qpc - ticks <= event[0] < probe_qpc]
        print("probe_window_ms=250")
        print(f"probe_window_draws={draw_count(window)}")
        print(f"baseline_window_draws={draw_count(baseline)}")
        return 0

    gpu_selection = next((event for event in events if event[2] == 30), None)
    print("primary_gpu_thread=" +
          (str(gpu_selection[4]) if gpu_selection else "not_selected"))

    for label, enter_type, exit_type in (
        ("dispatch", 3, 4), ("probe_dispatch", 5, 6),
        ("worker", 20, 21), ("scheduler", 22, 23),
        ("submit", 24, 25),
    ):
        durations = pair_durations(events, enter_type, exit_type, frequency)
        if durations:
            print(f"{label}_pairs={len(durations)} "
                  f"min_ms={min(durations):.4f} "
                  f"avg_ms={sum(durations) / len(durations):.4f} "
                  f"max_ms={max(durations):.4f}")

    plugin_submit_exits = [event for event in events
                           if event[2] == 25 and event[4] == 1]
    returned_tasks = {event[5] for event in plugin_submit_exits if event[5]}
    probe_owners = [event[4] for event in probes]
    confirmed_owners = [owner for owner in probe_owners
                        if owner in returned_tasks]
    print(f"plugin_submit_exits={len(plugin_submit_exits)}")
    print(f"plugin_returned_tasks={len(returned_tasks)}")
    print(f"probe_dispatches={len(probe_owners)}")
    print(f"confirmed_probe_dispatches={len(confirmed_owners)}")

    digests: dict[int, list[tuple[int, ...]]] = collections.defaultdict(list)
    for event in events:
        if event[2] == 27:
            digests[event[4]].append(event)
    for role in sorted(digests):
        values = digests[role]
        command_hashes = {event[6] for event in values}
        owner_hashes = {event[7] for event in values}
        context_hashes = {event[8] for event in values}
        sources = {signed32(event[5] & 0xFFFFFFFF) for event in values}
        print(f"digest[{ROLES.get(role, role)}] count={len(values)} "
              f"sources={','.join(map(str, sorted(sources)))} "
              f"command_hashes={len(command_hashes)} "
              f"owner_hashes={len(owner_hashes)} "
              f"context_hashes={len(context_hashes)}")
    real_hashes = {event[6] for event in digests.get(1, [])}
    confirmed_hashes = {event[6] for event in digests.get(3, [])}
    if real_hashes and confirmed_hashes:
        print("real_probe_command_hash_overlap=" +
              ("yes" if real_hashes & confirmed_hashes else "no"))

    stacks: dict[int, list[tuple[int, int]]] = collections.defaultdict(list)
    for event in events:
        if event[2] == 28:
            stacks[event[4]].append((event[5], event[6]))
    for role in sorted(stacks):
        ordered = sorted(stacks[role])
        rvas = ["external" if rva == 0xFFFFFFFFFFFFFFFF else f"0x{rva:X}"
                for _, rva in ordered]
        print(f"stack[{ROLES.get(role, role)}]=" + ",".join(rvas))

    confirmed_events = [event for event in probes if event[4] in returned_tasks]
    reference = confirmed_events[0] if confirmed_events else probes[0]
    ticks = max(1, frequency // 4)
    before = [event for event in events
              if reference[0] - ticks <= event[0] < reference[0]]
    after = [event for event in events
             if reference[0] <= event[0] <= reference[0] + ticks]
    print("confirmed_window_ms=250" if confirmed_events
          else "candidate_window_ms=250")
    print(f"window_before_draws={draw_count(before)}")
    print(f"window_after_draws={draw_count(after)}")
    print(f"window_before_targets="
          f"{sum(event[2] in (7, 8) for event in before)}")
    print(f"window_after_targets="
          f"{sum(event[2] in (7, 8) for event in after)}")
    print(f"frame_markers={totals[26]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
