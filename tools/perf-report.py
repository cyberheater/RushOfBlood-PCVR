#!/usr/bin/env python3
"""Sums up a performance trace written with perf_trace=1 (SHADPS4_PERF_TRACE=1).

    perf-report.py [log folder or perf_frames.csv] [--skip seconds] [--worst n]

Without a path it reads ..\\pc-vr\\user\\log next to this folder, then pc-vr\\user\\log under the
current one. It reads perf_frames.csv (one row a frame the game shows) and, where it is there,
perf_headset.csv (one row a refresh of the headset), and prints:
  - the frame rate and how steady it is (median, 90th/99th percentile, the longest frames),
  - where a frame's time goes on average (the game building it, the emulator's GPU thread
    translating it, the GPU drawing it, waits for the GPU, shader and pipeline compiles),
  - what the long frames (hitches) had in them,
  - for the headset: how often a refresh got a new frame, and whether frames were shown for an
    even number of refreshes (uneven = judder),
  - the worst seconds of the run,
  - and a verdict: what most likely holds the frame rate back, and what to try.
The first --skip seconds (default 15: start-up and loading) are left out.
"""
import csv
import math
import os
import statistics
import sys
from collections import Counter


def read_csv(path):
    """Rows of a trace file as dicts of floats, and the comment lines at its top."""
    comments = []
    with open(path, newline="", encoding="utf-8", errors="replace") as file:
        lines = []
        for line in file:
            if line.startswith("#"):
                comments.append(line[1:].strip())
            elif line.strip():
                lines.append(line)
    rows = []
    for row in csv.DictReader(lines):
        try:
            rows.append({key: float(value) for key, value in row.items() if key})
        except (TypeError, ValueError):
            continue  # a row cut short when the emulator was closed
    return rows, comments


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(math.ceil(fraction * len(ordered))) - 1))
    return ordered[index]


def mean(values):
    return statistics.fmean(values) if values else 0.0


def environment(comments):
    env = {}
    for line in comments:
        if line.startswith("environment:"):
            for pair in line[len("environment:"):].split():
                if "=" in pair:
                    key, value = pair.split("=", 1)
                    env[key] = value
    return env


def find_folder(argument):
    if argument:
        return os.path.dirname(argument) if argument.lower().endswith(".csv") else argument
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(here, "..", "..", "pc-vr", "user", "log"),
                      os.path.join(here, "..", "pc-vr", "user", "log"),
                      os.path.join("pc-vr", "user", "log"), "."):
        if os.path.exists(os.path.join(candidate, "perf_frames.csv")):
            return candidate
    sys.exit("No perf_frames.csv found: give the log folder (pc-vr\\user\\log) or the file.")


# Column -> what it is, for the breakdown.
PARTS = [
    ("submit_ms", "game building the frame (its own CPU work, up to handing it over)"),
    ("catchup_ms", "GPU thread still translating after the hand-over"),
    ("gputhread_cpu_ms", "CPU time of the emulator's GPU thread (translating PS4 commands)"),
    ("gpu_ms", "GPU time of the game's work (and the VR post-processing)"),
    ("gpu_present_ms", "GPU time of the window on the PC"),
    ("queued_ms", "frame ready, waiting for a headset refresh to take it"),
    ("gpuwait_ms", "waiting for the GPU to finish the frame before it is shown"),
    ("wait_throttle_ms", "emulator blocked: 8 submissions already in flight (GPU behind)"),
    ("wait_result_ms", "emulator blocked waiting for a GPU result (readback/sync)"),
    ("shader_ms", "translating new shaders"),
    ("pipeline_ms", "driver compiling new pipelines"),
    ("latency_ms", "first command of the frame until its flip (for reference)"),
]


def report_frames(rows, env, skip, worst):
    if not rows:
        print("perf_frames.csv has no rows.")
        return None
    start = rows[0]["t_s"] + skip
    body = [row for row in rows if row["t_s"] >= start and row["frame_ms"] > 0]
    if len(body) < 30:
        body = [row for row in rows if row["frame_ms"] > 0]
        print(f"(The run is short: the first {skip:.0f} s are kept in.)")
    if not body:
        print("No frames to look at.")
        return None
    duration = body[-1]["t_s"] - body[0]["t_s"] + body[0]["frame_ms"] / 1000.0
    times = [row["frame_ms"] for row in body]
    median = statistics.median(times)
    target = float(env.get("SHADPS4_VR_TARGET_FPS", 0) or 0)
    budget = 1000.0 / target if target > 0 else median

    print("=== Frames ===")
    print(f"{len(body)} frames over {duration:.1f} s: {len(body) / duration:.1f} fps on average"
          + (f" (target {target:.0f}, {budget:.1f} ms a frame)" if target > 0 else ""))
    print(f"frame time: median {median:.1f} ms, 90% under {percentile(times, 0.9):.1f} ms, "
          f"99% under {percentile(times, 0.99):.1f} ms, longest {max(times):.1f} ms")
    long_frames = [row for row in body if row["frame_ms"] > budget * 1.5]
    print(f"{len(long_frames)} frames ({100.0 * len(long_frames) / len(body):.1f}%) took over "
          f"1.5x the budget ({budget * 1.5:.1f} ms)")

    print("\n=== Where a frame's time goes (average / 95th percentile, ms) ===")
    averages = {}
    for column, meaning in PARTS:
        if column not in body[0]:
            continue
        values = [row[column] for row in body]
        averages[column] = mean(values)
        if max(values) <= 0.0:
            continue
        print(f"  {column:17} {mean(values):7.2f} / {percentile(values, 0.95):7.2f}   {meaning}")
    compiles = sum(row.get("shaders", 0) for row in body)
    pipelines = sum(row.get("pipelines", 0) for row in body)
    print(f"  draws {mean([r.get('draws', 0) for r in body]):.0f}, passes "
          f"{mean([r.get('passes', 0) for r in body]):.0f}, submits "
          f"{mean([r.get('submits', 0) for r in body]):.1f} a frame; {compiles:.0f} shaders and "
          f"{pipelines:.0f} pipelines compiled in all")
    if "gpu_timed" in body[0] and sum(r["gpu_timed"] for r in body) == 0:
        print("  (No GPU times: the GPU's queue gave no timestamps.)")

    print("\n=== Long frames (hitches) ===")
    if not long_frames:
        print("  none")
    else:
        causes = Counter()
        for row in long_frames:
            extra = row["frame_ms"] - budget
            compile_ms = row.get("shader_ms", 0) + row.get("pipeline_ms", 0)
            if compile_ms > 0.4 * extra:
                causes["shader/pipeline compiles"] += 1
            elif row.get("gpu_ms", 0) > budget:
                causes["GPU work over budget"] += 1
            elif row.get("gputhread_cpu_ms", 0) > budget:
                causes["GPU thread (translation) over budget"] += 1
            elif row.get("wait_result_ms", 0) > 0.4 * extra:
                causes["waiting for a GPU result"] += 1
            elif row.get("submit_ms", 0) > budget:
                causes["the game's own CPU work"] += 1
            else:
                causes["not explained by the trace (game logic, loading, the OS)"] += 1
        for cause, count in causes.most_common():
            print(f"  {count:5}  {cause}")
        print(f"  The {min(worst, len(long_frames))} longest:")
        for row in sorted(long_frames, key=lambda r: -r["frame_ms"])[:worst]:
            print(f"    t={row['t_s']:7.1f}s {row['frame_ms']:6.1f} ms: game {row.get('submit_ms', 0):.1f}, "
                  f"GPU thread {row.get('gputhread_cpu_ms', 0):.1f}, GPU {row.get('gpu_ms', 0):.1f}, "
                  f"compiles {row.get('shader_ms', 0) + row.get('pipeline_ms', 0):.1f} "
                  f"({row.get('shaders', 0):.0f}+{row.get('pipelines', 0):.0f}), "
                  f"result waits {row.get('wait_result_ms', 0):.1f}")

    print("\n=== Worst seconds ===")
    seconds = {}
    for row in body:
        seconds.setdefault(int(row["t_s"]), []).append(row)
    # (The first and last second are only partly in the run.)
    whole = {k: v for k, v in seconds.items() if min(seconds) < k < max(seconds)} or seconds
    ranked = sorted((s for s in whole.items() if len(s[1]) >= 2),
                    key=lambda s: (len(s[1]), -max(r["frame_ms"] for r in s[1])))
    for second, frames in ranked[:worst]:
        print(f"  {second:6} s: {len(frames):3} frames, longest {max(r['frame_ms'] for r in frames):6.1f} ms, "
              f"GPU {mean([r.get('gpu_ms', 0) for r in frames]):.1f}, "
              f"GPU thread {mean([r.get('gputhread_cpu_ms', 0) for r in frames]):.1f}, "
              f"game {mean([r.get('submit_ms', 0) for r in frames]):.1f} ms a frame")
    return {"budget": budget, "median": median, "averages": averages,
            "long": len(long_frames), "frames": len(body), "compiles": compiles + pipelines,
            "fps": len(body) / duration, "target": target}


def report_headset(rows, skip):
    if not rows:
        return None
    start = rows[0]["t_s"] + skip
    body = [row for row in rows if row["t_s"] >= start] or rows
    shown = [row for row in body if row["shown"]]
    hz = statistics.median([row["display_hz"] for row in body if row["display_hz"] > 0] or [0])
    period = 1000.0 / hz if hz > 0 else 0.0
    new = sum(1 for row in body if row["new_frame"])
    print("\n=== Headset ===")
    print(f"{len(body)} refreshes at {hz:.0f} Hz; {new} brought a new frame "
          f"({new / max(1, len(body)) * hz:.1f} new frames a second), "
          f"{len(body) - len(shown)} showed nothing")
    # How many refreshes each frame stayed up: even runs are smooth, a mix is judder.
    runs = []
    count = 0
    for row in body:
        if row["new_frame"]:
            if count:
                runs.append(count)
            count = 1
        elif count:
            count += 1
    holds = Counter(runs)
    if holds:
        usual, usual_count = holds.most_common(1)[0]
        uneven = 1.0 - usual_count / len(runs)
        print("frames held for " + ", ".join(f"{n} refresh{'es' if n > 1 else ''}: {c}"
                                            for n, c in sorted(holds.items())))
        print(f"{100 * uneven:.1f}% of frames were held for other than the usual {usual} "
              f"refresh{'es' if usual > 1 else ''}" + ("  <- judder" if uneven > 0.1 else ""))
    else:
        uneven = 0.0
    if period > 0:
        late = [row for row in body if row["interval_ms"] > period * 1.5]
        print(f"{len(late)} refreshes came late (over {period * 1.5:.1f} ms apart): the "
              f"runtime or the PC stalled; xrEndFrame {mean([r['end_ms'] for r in body]):.2f} ms, "
              f"handing a frame over {mean([r['copy_ms'] for r in body if r['new_frame']]):.2f} ms on average")
    return {"hz": hz, "uneven": uneven, "new_per_s": new / max(1, len(body)) * hz}


def verdict(frames, headset):
    print("\n=== Verdict ===")
    if frames is None:
        return
    budget = frames["budget"]
    a = frames["averages"]
    notes = []
    gpu = a.get("gpu_ms", 0) + a.get("gpu_present_ms", 0)
    if gpu > 0.85 * budget:
        notes.append(f"GPU-bound: the GPU needs {gpu:.1f} ms of the {budget:.1f} ms a frame. Lower "
                     "eye_width (Medium/Low in the window), layer_scale, fov, aniso or fxaa.")
    if a.get("gputhread_cpu_ms", 0) > 0.75 * budget:
        notes.append(f"Emulator-CPU-bound: the GPU thread spends {a['gputhread_cpu_ms']:.1f} ms a frame "
                     "translating the game's commands. Resolution settings will not help; this "
                     "needs work in the emulator (or a faster CPU).")
    if a.get("submit_ms", 0) > 0.75 * budget:
        notes.append(f"Game-CPU-bound: the game takes {a['submit_ms']:.1f} ms to build a frame. "
                     "Resolution settings will not help.")
    if a.get("wait_result_ms", 0) > 0.2 * budget:
        notes.append(f"The emulator waits {a['wait_result_ms']:.1f} ms a frame for GPU results "
                     "(readbacks): CPU and GPU take turns instead of working together.")
    if frames["compiles"] > 0 and frames["long"] > 0:
        notes.append(f"{frames['compiles']:.0f} shaders/pipelines were compiled during play: if the "
                     "hitches above are mostly compiles, turn on the pipeline cache "
                     "(pipeline_cache_enabled in user\\config.json) so later runs reuse them.")
    if headset is not None and headset["hz"] > 0:
        fps = frames["fps"]
        ratio = headset["hz"] / max(fps, 1.0)
        if headset["uneven"] > 0.1 or abs(ratio - round(ratio)) > 0.15:
            notes.append(f"Judder: about {fps:.0f} fps on a {headset['hz']:.0f} Hz display is "
                         f"{ratio:.2f} refreshes a frame. Use a refresh rate that is a whole "
                         "multiple of the frame rate (120 Hz for 60 fps, 90 Hz for 45).")
    busiest = max(gpu, a.get("gputhread_cpu_ms", 0), a.get("submit_ms", 0))
    if not any(n.endswith("bound") or "-bound:" in n for n in notes) and busiest < 0.6 * budget:
        notes.append(f"Headroom: the busiest part (GPU {gpu:.1f}, GPU thread "
                     f"{a.get('gputhread_cpu_ms', 0):.1f}, game {a.get('submit_ms', 0):.1f} ms) uses "
                     f"under 60% of the {budget:.1f} ms budget; a sharper picture (eye_width) is "
                     "affordable if the GPU figure stays well under the budget.")
    if not notes:
        notes.append("Nothing stands out: the frame rate holds and the headset gets frames evenly.")
    for note in notes:
        print(" - " + note)


def main():
    args = sys.argv[1:]
    skip, worst, path = 15.0, 8, None
    while args:
        arg = args.pop(0)
        if arg == "--skip":
            skip = float(args.pop(0))
        elif arg == "--worst":
            worst = int(args.pop(0))
        elif arg in ("-h", "--help"):
            print(__doc__)
            return
        else:
            path = arg
    folder = find_folder(path)
    frames_rows, comments = read_csv(os.path.join(folder, "perf_frames.csv"))
    env = environment(comments)
    for line in comments[:1]:
        print(line)
    if env:
        print("settings: " + " ".join(f"{k}={v}" for k, v in sorted(env.items())))
    print()
    frames = report_frames(frames_rows, env, skip, worst)
    headset_path = os.path.join(folder, "perf_headset.csv")
    headset = None
    if os.path.exists(headset_path):
        headset = report_headset(read_csv(headset_path)[0], skip)
    verdict(frames, headset)


if __name__ == "__main__":
    main()
