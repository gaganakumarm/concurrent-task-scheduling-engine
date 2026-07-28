#!/usr/bin/env python3
"""Validate Checkpoint 5.3 CSV evidence and generate summary/chart SVGs."""

import csv
import math
import statistics
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "results" / "profiling" / "raw"
SUMMARY = ROOT / "results" / "profiling" / "profiling-summary.csv"
IMAGES = ROOT / "docs" / "images" / "phase-5-profiling"

PROFILE_FILES = sorted(
    path for path in RAW.glob("*.csv")
    if ".workers." not in path.name
    and ".producers." not in path.name
    and not path.name.startswith("ordinary_")
)


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def median(rows, key):
    return statistics.median(float(row[key]) for row in rows)


def ratio(row, numerator, denominator):
    value = float(row[denominator])
    return float(row[numerator]) / value if value else 0.0


def validate():
    if len(PROFILE_FILES) != 16:
        raise RuntimeError(f"expected 16 profiling configurations, found {len(PROFILE_FILES)}")
    for path in PROFILE_FILES:
        rows = read_csv(path)
        if len(rows) != 5:
            raise RuntimeError(f"{path.name}: expected five measured rows")
        workers = read_csv(Path(str(path) + ".workers.csv"))
        producers = read_csv(Path(str(path) + ".producers.csv"))
        for row in rows:
            if row["correctness_passed"] != "true":
                raise RuntimeError(f"{path.name}: correctness failed")
            iteration = row["iteration"]
            worker_rows = [item for item in workers if item["iteration"] == iteration]
            producer_rows = [item for item in producers if item["iteration"] == iteration]
            if len(worker_rows) != int(row["workers"]):
                raise RuntimeError(f"{path.name}: incomplete worker evidence")
            if len(producer_rows) != int(row["producers"]):
                raise RuntimeError(f"{path.name}: incomplete producer evidence")
            executed = sum(int(item["tasks_executed"]) for item in worker_rows)
            dequeued = sum(int(item["tasks_dequeued"]) for item in worker_rows)
            attempted = sum(int(item["tasks_attempted"]) for item in producer_rows)
            accepted = sum(int(item["tasks_accepted"]) for item in producer_rows)
            rejected = sum(int(item["tasks_rejected"]) for item in producer_rows)
            expected = (
                int(row["tasks_executed"]),
                int(row["tasks_executed"]),
                int(row["tasks_attempted"]),
                int(row["tasks_accepted"]),
                int(row["tasks_rejected"]),
            )
            if (executed, dequeued, attempted, accepted, rejected) != expected:
                raise RuntimeError(f"{path.name}: companion totals disagree")


def summarize():
    ordinary = {
        path.stem.removeprefix("ordinary_"): read_csv(path)
        for path in sorted(RAW.glob("ordinary_*.csv"))
    }
    output = []
    for path in PROFILE_FILES:
        rows = read_csv(path)
        sample = rows[0]
        total = median(rows, "total_duration_ns")
        compute = median(rows, "callback_compute_total_ns")
        accounting = median(rows, "callback_accounting_total_ns")
        enqueue_wait = statistics.median(
            ratio(row, "enqueue_wait_count", "enqueue_lock_attempts") for row in rows
        )
        dequeue_wait = statistics.median(
            ratio(row, "dequeue_wait_count", "dequeue_lock_attempts") for row in rows
        )
        callback_profile = sample["callback_profile"]
        overhead = ""
        if path.stem in ("worker_noop_w4", "worker_light_w4"):
            ordinary_rows = ordinary["noop" if callback_profile == "noop" else "light"]
            ordinary_duration = median(ordinary_rows, "total_duration_ns")
            overhead = (total / ordinary_duration - 1.0) * 100.0
        output.append({
            "configuration": path.stem,
            "profile": callback_profile,
            "accounting_mode": sample["accounting_mode"],
            "workers": sample["workers"],
            "producers": sample["producers"],
            "capacity": sample["capacity"],
            "tasks": sample["tasks_attempted"],
            "iterations": len(rows),
            "median_throughput_tasks_per_second": median(rows, "throughput_tasks_per_second"),
            "median_total_duration_ns": total,
            "enqueue_wait_rate": enqueue_wait,
            "median_enqueue_wait_ns": statistics.median(
                float(row["enqueue_wait_total_ns"]) / max(1, int(row["enqueue_wait_count"]))
                for row in rows
            ),
            "dequeue_wait_rate": dequeue_wait,
            "median_dequeue_wait_ns": statistics.median(
                float(row["dequeue_wait_total_ns"]) / max(1, int(row["dequeue_wait_count"]))
                for row in rows
            ),
            "producer_lock_duration_share": median(rows, "enqueue_lock_wait_ns") / total,
            "worker_lock_duration_share": median(rows, "dequeue_lock_wait_ns") / total,
            "callback_compute_worker_time_share": compute / total,
            "callback_accounting_worker_time_share": accounting / total,
            "queue_full_observation_rate": median(rows, "queue_full_observations")
            / median(rows, "enqueue_lock_attempts"),
            "queue_empty_observation_rate": median(rows, "queue_empty_observations")
            / median(rows, "dequeue_lock_attempts"),
            "occupancy_event_mean": median(rows, "occupancy_event_mean"),
            "worker_distribution_cv": median(rows, "worker_task_cv"),
            "zero_task_workers": median(rows, "zero_task_workers"),
            "enqueue_predicate_false_wakeups": median(rows, "enqueue_predicate_false_wakeups"),
            "dequeue_predicate_false_wakeups": median(rows, "dequeue_predicate_false_wakeups"),
            "profiling_overhead_percent": overhead,
            "correctness": "true",
            "notes": "Timing shares are concurrent worker-time divided by wall-time and are non-additive.",
        })
    fields = list(output[0])
    SUMMARY.parent.mkdir(parents=True, exist_ok=True)
    with SUMMARY.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(output)
    return {row["configuration"]: row for row in output}


def svg_chart(filename, title, categories, series, y_label):
    width, height = 900, 500
    left, right, top, bottom = 85, 25, 60, 90
    plot_w, plot_h = width - left - right, height - top - bottom
    values = [value for _, points in series for value in points]
    ymax = max(values) if values else 1.0
    ymax = ymax * 1.12 if ymax > 0 else 1.0
    colors = ["#2364aa", "#d1495b", "#2a9d8f", "#f4a261"]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="{width / 2}" y="30" text-anchor="middle" font-family="sans-serif" font-size="20">{title}</text>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#333"/>',
        f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#333"/>',
    ]
    for tick in range(6):
        value = ymax * tick / 5
        y = top + plot_h - plot_h * tick / 5
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="#ddd"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 4:.1f}" text-anchor="end" font-family="sans-serif" font-size="12">{value:.3g}</text>')
    group = plot_w / max(1, len(categories))
    bar_width = group * 0.72 / len(series)
    for index, category in enumerate(categories):
        center = left + group * (index + 0.5)
        parts.append(f'<text x="{center:.1f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="sans-serif" font-size="12">{category}</text>')
        for series_index, (_, points) in enumerate(series):
            value = points[index]
            x = center - group * 0.36 + series_index * bar_width
            y = top + plot_h - value / ymax * plot_h
            parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_width - 2:.1f}" height="{top + plot_h - y:.1f}" fill="{colors[series_index]}"/>')
    parts.append(f'<text x="20" y="{top + plot_h / 2}" transform="rotate(-90 20 {top + plot_h / 2})" text-anchor="middle" font-family="sans-serif" font-size="13">{y_label}</text>')
    legend_x = left + 10
    for index, (name, _) in enumerate(series):
        x = legend_x + index * 185
        parts.append(f'<rect x="{x}" y="{height - 30}" width="14" height="14" fill="{colors[index]}"/>')
        parts.append(f'<text x="{x + 20}" y="{height - 18}" font-family="sans-serif" font-size="12">{name}</text>')
    parts.append("</svg>")
    (IMAGES / filename).write_text("\n".join(parts) + "\n", encoding="utf-8")


def charts(summary):
    IMAGES.mkdir(parents=True, exist_ok=True)
    workers = ["1", "2", "4", "8"]
    noop = [summary[f"worker_noop_w{w}"] for w in workers]
    light = [summary[f"worker_light_w{w}"] for w in workers]
    svg_chart("01-enqueue-wait-rate-workers.svg", "Enqueue wait rate vs workers", workers,
              [("noop", [100 * row["enqueue_wait_rate"] for row in noop]),
               ("light_cpu", [100 * row["enqueue_wait_rate"] for row in light])], "attempts entering wait (%)")
    svg_chart("02-dequeue-wait-rate-workers.svg", "Dequeue wait rate vs workers", workers,
              [("noop", [100 * row["dequeue_wait_rate"] for row in noop]),
               ("light_cpu", [100 * row["dequeue_wait_rate"] for row in light])], "attempts entering wait (%)")
    producer_rows = [summary[f"producer_noop_p{p}"] for p in ("1", "2")]
    producer_rows.append(summary["worker_noop_w4"])
    svg_chart("03-producer-lock-duration-producers.svg", "Producer lock duration vs producers",
              ["1", "2", "4"], [("enqueue lock total (ms)", [row["producer_lock_duration_share"] * row["median_total_duration_ns"] / 1e6 for row in producer_rows])], "median cumulative ms")
    svg_chart("04-worker-lock-duration-workers.svg", "Worker lock duration vs workers", workers,
              [("noop", [row["worker_lock_duration_share"] * row["median_total_duration_ns"] / 1e6 for row in noop]),
               ("light_cpu", [row["worker_lock_duration_share"] * row["median_total_duration_ns"] / 1e6 for row in light])], "median cumulative ms")
    capacities = ["1", "16", "64", "256"]
    caps = [summary[f"capacity_noop_c{c}"] if c != "64" else summary["worker_noop_w4"] for c in capacities]
    svg_chart("05-full-observations-capacity.svg", "Queue-full observations vs capacity", capacities,
              [("full observations", [row["queue_full_observation_rate"] * 100000 for row in caps])], "median observations")
    svg_chart("06-occupancy-capacity.svg", "Event-sampled occupancy vs capacity", capacities,
              [("mean occupancy", [row["occupancy_event_mean"] for row in caps])], "Tasks (event sampled)")
    worker_rows = read_csv(RAW / "worker_noop_w4.csv.workers.csv")
    by_worker = []
    for worker in range(4):
        by_worker.append(statistics.median(int(row["tasks_executed"]) for row in worker_rows if int(row["worker_index"]) == worker))
    svg_chart("07-tasks-per-worker.svg", "Tasks executed per worker: noop, 4 workers", ["0", "1", "2", "3"],
              [("median tasks", by_worker)], "Tasks")
    svg_chart("08-worker-cv.svg", "Worker Task-distribution CV", workers,
              [("noop", [row["worker_distribution_cv"] for row in noop]),
               ("light_cpu", [row["worker_distribution_cv"] for row in light])], "coefficient of variation")
    profiles = [summary["worker_noop_w4"], summary["worker_light_w4"], summary["callback_medium"]]
    svg_chart("09-callback-components.svg", "Callback computation vs accounting", ["noop", "light", "medium"],
              [("compute worker-ms", [row["callback_compute_worker_time_share"] * row["median_total_duration_ns"] / 1e6 for row in profiles]),
               ("accounting worker-ms", [row["callback_accounting_worker_time_share"] * row["median_total_duration_ns"] / 1e6 for row in profiles])], "median cumulative ms")
    svg_chart("10-accounting-throughput.svg", "Standard vs partitioned accounting throughput", ["noop", "light"],
              [("standard", [summary["worker_noop_w4"]["median_throughput_tasks_per_second"], summary["worker_light_w4"]["median_throughput_tasks_per_second"]]),
               ("partitioned", [summary["accounting_noop_partitioned"]["median_throughput_tasks_per_second"], summary["accounting_light_partitioned"]["median_throughput_tasks_per_second"]])], "Tasks/s")
    svg_chart("11-instrumentation-overhead.svg", "Profiling instrumentation overhead", ["noop", "light"],
              [("overhead", [summary["worker_noop_w4"]["profiling_overhead_percent"], summary["worker_light_w4"]["profiling_overhead_percent"]])], "median duration overhead (%)")
    selected = [summary["capacity_noop_c1"], summary["worker_noop_w4"], summary["worker_noop_w8"]]
    svg_chart("12-predicate-false-wakeups.svg", "Predicate-false wake-ups", ["capacity 1", "w4/c64", "w8/c64"],
              [("enqueue", [row["enqueue_predicate_false_wakeups"] for row in selected]),
               ("dequeue", [row["dequeue_predicate_false_wakeups"] for row in selected])], "median wake-ups")


def main():
    validate()
    summary = summarize()
    charts(summary)
    print(f"validated {len(PROFILE_FILES)} configurations; wrote {SUMMARY} and 12 charts")


if __name__ == "__main__":
    main()
