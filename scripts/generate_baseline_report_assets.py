"""Validate Performance Evaluation baseline CSV files and generate summary data and SVGs."""

from __future__ import annotations

import csv
import math
import statistics
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "results" / "baseline" / "raw"
SUMMARY = ROOT / "results" / "baseline" / "baseline-summary.csv"
CHARTS = ROOT / "docs" / "images" / "performance"

EXPECTED_HEADER = [
    "timestamp_utc",
    "scenario",
    "callback_profile",
    "mode",
    "iteration",
    "workers",
    "producers",
    "capacity",
    "tasks_attempted",
    "tasks_accepted",
    "tasks_executed",
    "tasks_rejected",
    "submit_duration_ns",
    "shutdown_duration_ns",
    "join_duration_ns",
    "total_duration_ns",
    "throughput_tasks_per_second",
    "mean_submit_latency_ns",
    "p50_submit_latency_ns",
    "p95_submit_latency_ns",
    "min_submit_latency_ns",
    "max_submit_latency_ns",
    "mean_end_to_end_latency_ns",
    "p50_end_to_end_latency_ns",
    "p95_end_to_end_latency_ns",
    "correctness_passed",
]

RUNS = {
    "workers_noop_w1_p4_c64_t100000.csv": ("worker_scaling", "primary"),
    "workers_noop_w2_p4_c64_t100000.csv": ("worker_scaling", "primary"),
    "workers_noop_w4_p4_c64_t100000.csv": ("worker_scaling", "primary"),
    "workers_noop_w8_p4_c64_t100000.csv": ("worker_scaling", "primary"),
    "producers_noop_w4_p1_c64_t100000.csv": (
        "producer_scaling",
        "primary",
    ),
    "producers_noop_w4_p2_c64_t100000.csv": (
        "producer_scaling",
        "primary",
    ),
    "capacity_noop_w4_p4_c1_t100000.csv": ("capacity_scaling", "primary"),
    "capacity_noop_w4_p4_c16_t100000.csv": ("capacity_scaling", "primary"),
    "capacity_noop_w4_p4_c256_t100000.csv": (
        "capacity_scaling",
        "primary",
    ),
    "profiles_light_cpu_w4_p4_c64_t100000.csv": (
        "callback_profiles",
        "primary",
    ),
    "profiles_medium_cpu_w4_p4_c64_t100000.csv": (
        "callback_profiles",
        "primary",
    ),
    "workers_light_cpu_w1_p4_c64_t100000.csv": (
        "light_worker_scaling",
        "primary",
    ),
    "workers_light_cpu_w2_p4_c64_t100000.csv": (
        "light_worker_scaling",
        "primary",
    ),
    "workers_light_cpu_w8_p4_c64_t100000.csv": (
        "light_worker_scaling",
        "primary",
    ),
    "repeat_noop_w4_p4_c64_t100000.csv": ("stability_repeat", "repeat"),
    "repeat_light_cpu_w4_p4_c64_t100000.csv": (
        "stability_repeat",
        "repeat",
    ),
    "controlled_blocking_w1_p1_c1_t1000.csv": (
        "controlled_backpressure",
        "behavioral",
    ),
}


def nearest_rank(values: list[float], percentile: int) -> float:
    ordered = sorted(values)
    index = math.ceil(percentile * len(ordered) / 100) - 1
    return ordered[index]


def fmt(value: float | None) -> str:
    return "" if value is None else f"{value:.6f}"


def load_and_validate(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != EXPECTED_HEADER:
            raise ValueError(f"{path.name}: unexpected CSV header")
        rows = list(reader)
    if len(rows) != 10:
        raise ValueError(f"{path.name}: expected 10 rows, found {len(rows)}")
    constant_fields = [
        "scenario",
        "callback_profile",
        "mode",
        "workers",
        "producers",
        "capacity",
        "tasks_attempted",
    ]
    for field in constant_fields:
        if len({row[field] for row in rows}) != 1:
            raise ValueError(f"{path.name}: inconsistent {field}")
    if {int(row["iteration"]) for row in rows} != set(range(1, 11)):
        raise ValueError(f"{path.name}: iteration sequence is invalid")
    integer_fields = [
        "iteration",
        "workers",
        "producers",
        "capacity",
        "tasks_attempted",
        "tasks_accepted",
        "tasks_executed",
        "tasks_rejected",
        "submit_duration_ns",
        "shutdown_duration_ns",
        "join_duration_ns",
        "total_duration_ns",
        "p50_submit_latency_ns",
        "p95_submit_latency_ns",
        "min_submit_latency_ns",
        "max_submit_latency_ns",
        "p50_end_to_end_latency_ns",
        "p95_end_to_end_latency_ns",
    ]
    float_fields = [
        "throughput_tasks_per_second",
        "mean_submit_latency_ns",
        "mean_end_to_end_latency_ns",
    ]
    for row in rows:
        for field in integer_fields:
            if row[field] == "":
                raise ValueError(f"{path.name}: empty validated field {field}")
            int(row[field])
        for field in float_fields:
            if row[field] == "" or not math.isfinite(float(row[field])):
                raise ValueError(f"{path.name}: invalid numeric {field}")
        attempted = int(row["tasks_attempted"])
        accepted = int(row["tasks_accepted"])
        executed = int(row["tasks_executed"])
        rejected = int(row["tasks_rejected"])
        if accepted != executed or attempted != accepted + rejected:
            raise ValueError(f"{path.name}: accounting mismatch")
        if rejected != 0 or row["correctness_passed"] != "true":
            raise ValueError(f"{path.name}: correctness gate failed")
    return rows


def make_summary(
    filename: str,
    category: str,
    run_kind: str,
    rows: list[dict[str, str]],
) -> dict[str, str]:
    throughput = [float(row["throughput_tasks_per_second"]) for row in rows]
    submit_p50 = [float(row["p50_submit_latency_ns"]) for row in rows]
    submit_p95 = [float(row["p95_submit_latency_ns"]) for row in rows]
    e2e_p50 = [float(row["p50_end_to_end_latency_ns"]) for row in rows]
    e2e_p95 = [float(row["p95_end_to_end_latency_ns"]) for row in rows]
    shutdown = [float(row["shutdown_duration_ns"]) for row in rows]
    join = [float(row["join_duration_ns"]) for row in rows]
    total = [float(row["total_duration_ns"]) for row in rows]
    first = rows[0]
    return {
        "scenario": category,
        "run_kind": run_kind,
        "source_file": filename,
        "callback_profile": first["callback_profile"],
        "workers": first["workers"],
        "producers": first["producers"],
        "capacity": first["capacity"],
        "tasks": first["tasks_attempted"],
        "measured_iterations": str(len(rows)),
        "min_throughput_tasks_per_second": fmt(min(throughput)),
        "max_throughput_tasks_per_second": fmt(max(throughput)),
        "median_throughput_tasks_per_second": fmt(nearest_rank(throughput, 50)),
        "mean_throughput_tasks_per_second": fmt(statistics.fmean(throughput)),
        "p95_throughput_tasks_per_second": fmt(nearest_rank(throughput, 95)),
        "throughput_population_stddev": fmt(statistics.pstdev(throughput)),
        "median_of_iteration_p50_submit_latency_ns": fmt(
            nearest_rank(submit_p50, 50)
        ),
        "median_of_iteration_p95_submit_latency_ns": fmt(
            nearest_rank(submit_p95, 50)
        ),
        "median_of_iteration_p50_end_to_end_latency_ns": fmt(
            nearest_rank(e2e_p50, 50)
        ),
        "median_of_iteration_p95_end_to_end_latency_ns": fmt(
            nearest_rank(e2e_p95, 50)
        ),
        "median_shutdown_latency_ns": fmt(nearest_rank(shutdown, 50)),
        "median_join_latency_ns": fmt(nearest_rank(join, 50)),
        "median_total_duration_ns": fmt(nearest_rank(total, 50)),
        "speedup": "",
        "efficiency": "",
        "correctness": "true",
        "notes": (
            "median latency columns summarize per-iteration percentiles; "
            + run_kind
        ),
    }


def add_scaling(
    rows: list[dict[str, str]],
    category: str,
    extra_profile: str | None = None,
) -> None:
    selected = [
        row
        for row in rows
        if row["run_kind"] == "primary"
        and (
            row["scenario"] == category
            or (
                extra_profile is not None
                and row["scenario"] == "callback_profiles"
                and row["callback_profile"] == extra_profile
            )
        )
    ]
    baseline = next(row for row in selected if row["workers"] == "1")
    baseline_value = float(baseline["median_throughput_tasks_per_second"])
    for row in selected:
        speedup = (
            float(row["median_throughput_tasks_per_second"]) / baseline_value
        )
        row["speedup"] = fmt(speedup)
        row["efficiency"] = fmt(speedup / int(row["workers"]))


def svg_chart(
    path: Path,
    title: str,
    x_label: str,
    y_label: str,
    labels: list[str],
    series: list[tuple[str, list[float]]],
) -> None:
    width, height = 900, 520
    left, right, top, bottom = 95, 30, 65, 85
    plot_w = width - left - right
    plot_h = height - top - bottom
    maximum = max(max(values) for _, values in series)
    maximum = maximum * 1.1 if maximum > 0 else 1.0
    colors = ["#2563eb", "#dc2626", "#059669"]
    group_width = plot_w / len(labels)
    bar_width = group_width * 0.7 / len(series)
    items = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="{width / 2}" y="30" text-anchor="middle" '
        f'font-family="sans-serif" font-size="20">{title}</text>',
        f'<line x1="{left}" y1="{top}" x2="{left}" '
        f'y2="{top + plot_h}" stroke="black"/>',
        f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" '
        f'y2="{top + plot_h}" stroke="black"/>',
    ]
    for tick in range(6):
        value = maximum * tick / 5
        y = top + plot_h - plot_h * tick / 5
        items.append(
            f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" '
            'y2="{:.2f}" stroke="#e5e7eb"/>'.format(y)
        )
        items.append(
            f'<text x="{left - 8}" y="{y + 4:.2f}" text-anchor="end" '
            f'font-family="sans-serif" font-size="11">{value:.2f}</text>'
        )
    for label_index, label in enumerate(labels):
        center = left + group_width * (label_index + 0.5)
        items.append(
            f'<text x="{center:.2f}" y="{top + plot_h + 22}" '
            f'text-anchor="middle" font-family="sans-serif" '
            f'font-size="12">{label}</text>'
        )
        for series_index, (name, values) in enumerate(series):
            value = values[label_index]
            height_value = plot_h * value / maximum
            x = (
                center
                - (bar_width * len(series) / 2)
                + series_index * bar_width
            )
            y = top + plot_h - height_value
            items.append(
                f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_width:.2f}" '
                f'height="{height_value:.2f}" fill="{colors[series_index]}">'
                f"<title>{name}: {value:.6f}</title></rect>"
            )
    items.extend(
        [
            f'<text x="{left + plot_w / 2}" y="{height - 25}" '
            f'text-anchor="middle" font-family="sans-serif" '
            f'font-size="13">{x_label}</text>',
            f'<text x="20" y="{top + plot_h / 2}" text-anchor="middle" '
            f'font-family="sans-serif" font-size="13" '
            f'transform="rotate(-90 20 {top + plot_h / 2})">{y_label}</text>',
        ]
    )
    legend_x = left + 10
    for index, (name, _) in enumerate(series):
        x = legend_x + index * 180
        items.append(
            f'<rect x="{x}" y="42" width="12" height="12" '
            f'fill="{colors[index]}"/>'
        )
        items.append(
            f'<text x="{x + 18}" y="53" font-family="sans-serif" '
            f'font-size="11">{name}</text>'
        )
    items.append("</svg>")
    path.write_text("\n".join(items) + "\n", encoding="utf-8")


def selected(
    summaries: list[dict[str, str]], category: str
) -> list[dict[str, str]]:
    return sorted(
        [
            row
            for row in summaries
            if row["scenario"] == category and row["run_kind"] == "primary"
        ],
        key=lambda row: (
            int(row["workers"]),
            int(row["producers"]),
            int(row["capacity"]),
        ),
    )


def main() -> None:
    actual = {path.name for path in RAW.glob("*.csv")}
    expected = set(RUNS)
    if actual != expected:
        raise ValueError(
            f"raw inventory mismatch: missing={expected-actual}, extra={actual-expected}"
        )
    summaries = []
    accepted_total = 0
    for filename, (category, run_kind) in RUNS.items():
        rows = load_and_validate(RAW / filename)
        accepted_total += sum(int(row["tasks_accepted"]) for row in rows)
        summaries.append(
            make_summary(filename, category, run_kind, rows)
        )
    add_scaling(summaries, "worker_scaling")
    add_scaling(summaries, "light_worker_scaling", "light_cpu")

    SUMMARY.parent.mkdir(parents=True, exist_ok=True)
    with SUMMARY.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)

    CHARTS.mkdir(parents=True, exist_ok=True)
    workers = selected(summaries, "worker_scaling")
    light_workers = selected(summaries, "light_worker_scaling")
    producers = selected(summaries, "producer_scaling")
    base_w4 = next(row for row in workers if row["workers"] == "4")
    producers.append(base_w4 | {"scenario": "producer_scaling"})
    producers.sort(key=lambda row: int(row["producers"]))
    capacities = selected(summaries, "capacity_scaling")
    capacities.append(base_w4 | {"scenario": "capacity_scaling"})
    capacities.sort(key=lambda row: int(row["capacity"]))
    profiles = [
        base_w4 | {"scenario": "callback_profiles"},
        *selected(summaries, "callback_profiles"),
    ]
    profiles.sort(
        key=lambda row: ["noop", "light_cpu", "medium_cpu"].index(
            row["callback_profile"]
        )
    )
    light_workers.append(
        next(row for row in profiles if row["callback_profile"] == "light_cpu")
    )
    light_workers.sort(key=lambda row: int(row["workers"]))

    svg_chart(
        CHARTS / "noop-throughput-by-workers.svg",
        "No-op throughput versus worker count",
        "Workers",
        "Tasks/second (axis starts at zero)",
        [row["workers"] for row in workers],
        [(
            "Median throughput",
            [float(row["median_throughput_tasks_per_second"]) for row in workers],
        )],
    )
    svg_chart(
        CHARTS / "noop-speedup-by-workers.svg",
        "No-op speedup versus worker count",
        "Workers",
        "Speedup relative to one worker",
        [row["workers"] for row in workers],
        [("Measured speedup", [float(row["speedup"]) for row in workers])],
    )
    svg_chart(
        CHARTS / "noop-efficiency-by-workers.svg",
        "No-op worker efficiency",
        "Workers",
        "Efficiency (fraction)",
        [row["workers"] for row in workers],
        [("Measured efficiency", [float(row["efficiency"]) for row in workers])],
    )
    svg_chart(
        CHARTS / "throughput-by-producers.svg",
        "No-op throughput versus producer count",
        "Producers",
        "Tasks/second (axis starts at zero)",
        [row["producers"] for row in producers],
        [(
            "Median throughput",
            [float(row["median_throughput_tasks_per_second"]) for row in producers],
        )],
    )
    svg_chart(
        CHARTS / "throughput-by-capacity.svg",
        "No-op throughput versus queue capacity",
        "Queue capacity",
        "Tasks/second (axis starts at zero)",
        [row["capacity"] for row in capacities],
        [(
            "Median throughput",
            [float(row["median_throughput_tasks_per_second"]) for row in capacities],
        )],
    )
    svg_chart(
        CHARTS / "submit-p95-by-capacity.svg",
        "Submission p95 latency versus queue capacity",
        "Queue capacity",
        "Median of per-iteration p95 (ns)",
        [row["capacity"] for row in capacities],
        [(
            "Submission p95",
            [
                float(row["median_of_iteration_p95_submit_latency_ns"])
                for row in capacities
            ],
        )],
    )
    svg_chart(
        CHARTS / "throughput-by-callback-profile.svg",
        "Throughput by callback profile",
        "Callback profile",
        "Tasks/second (axis starts at zero)",
        [row["callback_profile"] for row in profiles],
        [(
            "Median throughput",
            [float(row["median_throughput_tasks_per_second"]) for row in profiles],
        )],
    )
    svg_chart(
        CHARTS / "light-throughput-by-workers.svg",
        "Light CPU throughput versus worker count",
        "Workers",
        "Tasks/second (axis starts at zero)",
        [row["workers"] for row in light_workers],
        [(
            "Median throughput",
            [
                float(row["median_throughput_tasks_per_second"])
                for row in light_workers
            ],
        )],
    )
    svg_chart(
        CHARTS / "e2e-p95-by-callback-profile.svg",
        "End-to-end p95 latency by callback profile",
        "Callback profile",
        "Median of per-iteration p95 (ns)",
        [row["callback_profile"] for row in profiles],
        [(
            "End-to-end p95",
            [
                float(row["median_of_iteration_p95_end_to_end_latency_ns"])
                for row in profiles
            ],
        )],
    )
    svg_chart(
        CHARTS / "shutdown-join-by-callback-profile.svg",
        "Shutdown and join median latency",
        "Callback profile",
        "Latency (ns, axis starts at zero)",
        [row["callback_profile"] for row in profiles],
        [
            (
                "Shutdown median",
                [float(row["median_shutdown_latency_ns"]) for row in profiles],
            ),
            (
                "Join median",
                [float(row["median_join_latency_ns"]) for row in profiles],
            ),
        ],
    )
    print(
        f"validated_csv={len(RUNS)} rows={len(RUNS) * 10} "
        f"accepted={accepted_total} charts=10"
    )


if __name__ == "__main__":
    main()
