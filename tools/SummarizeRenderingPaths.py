#!/usr/bin/env python3
"""Validate renderer samples, publish compact evidence and an optional GPU chart.

Python standard library suffices for JSON/CSV; --plot also needs matplotlib.
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def distribution(values):
    require(bool(values), "Empty timing distribution")
    require(all(isinstance(x, (int, float)) and math.isfinite(x) and x > 0
                for x in values), "Invalid timing value")
    ordered = sorted(values)
    return {"count": len(values), "median": statistics.median(values),
            "p95": ordered[math.ceil(.95 * len(values)) - 1],
            "mean": statistics.mean(values)}


def source_gpu_samples(frames, first, last):
    """Resolve GPU queries by the rendered source frame, including drain rows."""
    result = {}
    for frame in frames:
        view = frame["views"]["game"]
        source = view["gpuResolvedFrameId"]
        if source is not None and first <= source <= last:
            require(source not in result, f"Duplicate GPU source frame {source}")
            require(view["gpuTotalMs"] is not None, f"Missing GPU timing {source}")
            result[source] = view["gpuTotalMs"]
    require(set(result) == set(range(first, last + 1)), "Missing completed GPU source frames")
    distribution(list(result.values()))
    return result


def present_interval(frame, previous):
    """Older public profilers have no Present-return timestamps; leave them absent."""
    current = frame.get("pipeline", {}).get("steadyTimelineNanoseconds", {})
    prior = previous.get("pipeline", {}).get("steadyTimelineNanoseconds", {})
    if current.get("timingContractVersion") != 2 or prior.get("timingContractVersion") != 2:
        return None
    require(isinstance(current.get("presentReturned"), int)
            and isinstance(prior.get("presentReturned"), int), "Missing Present timestamp")
    interval = (current["presentReturned"] - prior["presentReturned"]) / 1e6
    require(interval > 0, "Present timestamps must advance")
    return interval


def summarize_run(index, run):
    directory = Path(run["directory"])
    require(run["status"] == "captured", "Run did not complete")
    for artifact in run["artifacts"]:
        path = (directory / artifact["path"]).resolve()
        require(path.is_relative_to(directory.resolve()), "Artifact escapes run directory")
        require(sha256(path) == artifact["sha256"].upper(), f"Artifact hash mismatch: {path}")
    records = [json.loads(line) for line in (directory / "frames.jsonl").read_text(
        encoding="utf-8-sig").splitlines() if line.strip()]
    header, footer = records[0], records[-1]
    total = index["warmupFrames"] + index["sampleFrames"] + index["drainFrames"]
    require(header.get("format") == "PrismFrameProfiler" and header.get("version") == 1,
            "Unsupported profiler format")
    require(footer.get("status") == "complete" and footer.get("completedFrames") == total,
            "Incomplete profiler report")
    metadata = header["metadata"]
    identity = metadata["identity"]["identity"]
    require(identity["graphicsApi"] == index["backend"], "Backend mismatch")
    require(not metadata["editorEnabled"] and metadata["headless"]
            and not metadata["validationRequested"] and metadata["deterministic"],
            "Unexpected editor/validation/input policy")
    require((metadata["width"], metadata["height"]) == (index["width"], index["height"]),
            "Resolution mismatch")
    frames = records[1:-1]
    require([f.get("frameId") for f in frames] == list(range(1, total + 1)),
            "Missing or repeated frame IDs")
    first, last = index["warmupFrames"] + 1, index["warmupFrames"] + index["sampleFrames"]
    gpu = source_gpu_samples(frames, first, last)
    samples = []
    for frame in frames[first - 1:last]:
        require(frame["actualLevel"] == "basic" and frame["activeViewMask"] == 1,
                "Unexpected profiling level or active views")
        view, pacing = frame["views"]["game"], frame["framePacing"]
        require(view["active"] and view["rendered"] and
                (view["width"], view["height"]) == (index["width"], index["height"]),
                "Game output was not rendered at the requested extent")
        require(pacing["available"] and pacing["profile"] == "benchmark"
                and pacing["effectivePresentation"] == "immediate"
                and pacing["targetFps"] is None and not pacing["transitionPending"],
                "Uncapped immediate presentation did not take effect")
        if index["backend"] == "d3d12":
            require(pacing["syncInterval"] == 0, "VSync is enabled")
        interval = present_interval(frame, frames[frame["frameId"] - 2])
        samples.append({"path": run["path"], "lightCount": run["lightCount"],
                        "repetition": run["repetition"], "frameId": frame["frameId"],
                        "gpuMs": gpu[frame["frameId"]], "applicationLoopMs": frame["editorLoopMs"],
                        "presentIntervalMs": interval})
    metrics = {key: distribution([s[key] for s in samples])
               for key in ("gpuMs", "applicationLoopMs")}
    present_intervals = [s["presentIntervalMs"] for s in samples if s["presentIntervalMs"] is not None]
    require(len(present_intervals) in (0, len(samples)), "Mixed Present timing availability")
    metrics["presentIntervalMs"] = distribution(present_intervals) if present_intervals else None
    return {"label": run["label"], "path": run["path"], "lightCount": run["lightCount"],
            "repetition": run["repetition"], "order": run["order"], "metrics": metrics,
            "framesSha256": sha256(directory / "frames.jsonl"),
            "pacing": frames[first - 1]["framePacing"]}, identity, samples


def plot(summary, output):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    names = {"forward": "Forward", "forward-plus": "Forward+", "deferred": "Deferred"}
    colors = {"forward": "#62a5f7", "forward-plus": "#2cc5ac", "deferred": "#b798f8"}
    counts = sorted({case["lightCount"] for case in summary["cases"]})
    fig, axes = plt.subplots(1, len(counts), figsize=(11.8, 4.5), squeeze=False)
    fig.patch.set_facecolor("#101827")
    maximum = max(c["gpuMedianMs"]["maximum"] for c in summary["cases"])
    for ax, count in zip(axes[0], counts):
        cases = [c for c in summary["cases"] if c["lightCount"] == count]
        ax.set_facecolor("#101827")
        values = [c["gpuMedianMs"]["median"] for c in cases]
        errors = [[v - c["gpuMedianMs"]["minimum"] for c, v in zip(cases, values)],
                  [c["gpuMedianMs"]["maximum"] - v for c, v in zip(cases, values)]]
        bars = ax.bar([names[c["path"]] for c in cases], values, width=.54,
                      color=[colors[c["path"]] for c in cases], yerr=errors,
                      capsize=5, error_kw={"ecolor": "#e2e8f0", "elinewidth": 1.3})
        for bar, value in zip(bars, values):
            ax.text(bar.get_x() + bar.get_width()/2, value + maximum*.12,
                    f"{value:.3f} ms", ha="center", color="white", fontsize=12)
        ax.set_title(f"{count} point lights", color="white", fontsize=14, pad=16)
        ax.set_ylim(0, maximum * 1.4)
        ax.set_ylabel("Game renderer GPU time (ms)", color="#cad5e5")
        ax.tick_params(colors="#cad5e5", labelsize=11)
        ax.grid(axis="y", color="#293446", alpha=.65)
        ax.set_axisbelow(True)
        for spine in ax.spines.values():
            spine.set_visible(False)
    identity = summary["identity"]
    fig.suptitle("PrismRender / Rendering Path Performance", color="white", fontsize=18, y=.98)
    fig.text(.5, .86, f'{identity["adapterName"]}  |  {summary["backend"].upper()}  |  '
             f'{summary["width"]} x {summary["height"]}', ha="center", color="#b4c2d7", fontsize=10)
    fig.text(.5, .04, f'Median of {summary["repetitions"]} runs; whiskers = run-median min/max. '
             'Lower is faster.\nForward is omitted above its 4-point-light limit.',
             ha="center", color="#b4c2d7", fontsize=10)
    fig.subplots_adjust(top=.74, bottom=.2, left=.075, right=.985, wspace=.24)
    fig.savefig(output, dpi=170, facecolor=fig.get_facecolor())
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Measurement directory containing index.json")
    parser.add_argument("--output", type=Path, required=True, help="Directory for published JSON/CSV/PNG")
    parser.add_argument("--plot", action="store_true")
    args = parser.parse_args()
    index = read_json(args.input / "index.json")
    require(index["format"] == "PrismRenderingPathMeasurement" and index["status"] == "captured",
            "Measurement did not complete")
    require(len(index["runs"]) == len(index["cases"]) * index["repetitions"], "Incomplete run matrix")
    runs, samples, identity = [], [], None
    for run in index["runs"]:
        result, hardware, values = summarize_run(index, run)
        if identity is None:
            identity = hardware
        require(hardware == identity, "Hardware/build identity changed across runs")
        runs.append(result)
        samples.extend(values)
    cases = []
    for case in index["cases"]:
        selected = [r for r in runs if (r["path"], r["lightCount"]) == (case["path"], case["lightCount"])]
        require(len(selected) == index["repetitions"], "Missing repeated case")
        require(len({r["repetition"] for r in selected}) == index["repetitions"], "Repeated run identity")
        result = dict(case)
        for name, metric, statistic in [("gpuMedianMs", "gpuMs", "median"),
                                        ("gpuP95Ms", "gpuMs", "p95"),
                                        ("applicationLoopMedianMs", "applicationLoopMs", "median"),
                                        ("presentIntervalMedianMs", "presentIntervalMs", "median")]:
            available = [r["metrics"][metric] for r in selected if r["metrics"][metric] is not None]
            require(len(available) in (0, len(selected)), "Mixed timing availability across runs")
            if not available:
                result[name] = None
                continue
            values = [metric_value[statistic] for metric_value in available]
            result[name] = {"median": statistics.median(values), "minimum": min(values), "maximum": max(values)}
        cases.append(result)
    summary = {"format": "PrismRenderingPathSummary", "version": 1,
               **{k: index[k] for k in ("backend", "scene", "width", "height", "warmupFrames",
                                        "sampleFrames", "drainFrames", "repetitions", "binarySha256")},
               "identity": identity,
               "startedUtc": min(r["startedUtc"] for r in index["runs"]),
               "finishedUtc": max(r["finishedUtc"] for r in index["runs"]),
               "quantiles": "median-average-middle; p95-nearest-rank",
               "aggregation": "median of per-run statistics; min/max show inter-run spread",
               "gpuAssociation": "source frame ID; warmup excluded; drain rows resolve measurement tail",
               "conditions": {"pointLights": sorted({c["lightCount"] for c in cases}), "objects": 84, "subjects": 48,
                              "editor": False, "validation": False, "profiling": "basic",
                              "GTAO": False, "TAA": False, "SSR": False, "localLightShadows": False,
                              "CSM": True, "IBL": True, "Bloom": True,
                              "gpuClocksLocked": False,
                              "rhiExecution": identity.get("rhiExecutionMode", "legacy-native")},
               "cases": cases, "runs": runs}
    args.output.mkdir(parents=True, exist_ok=True)
    csv_path = args.output / "rendering-path-samples.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(samples[0]))
        writer.writeheader()
        writer.writerows(samples)
    summary["samplesSha256"] = sha256(csv_path)
    summary_path = args.output / "rendering-path-performance.json"
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.plot:
        plot(summary, args.output / "rendering-path-performance.png")
    print(json.dumps({"identity": identity, "cases": cases}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
