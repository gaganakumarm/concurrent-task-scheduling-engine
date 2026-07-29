#!/usr/bin/env python3
"""Validate and summarize the Verification step 5.4 control/candidate evidence."""

import csv
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "results" / "optimization" / "raw"
OUT = ROOT / "results" / "optimization" / "optimization-summary.csv"
CHARTS = ROOT / "results" / "optimization" / "charts"


def read(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def med(rows, field):
    return statistics.median(float(row[field]) for row in rows)


def stats(rows, field):
    values = [float(row[field]) for row in rows]
    mean = statistics.mean(values)
    return min(values), max(values), statistics.pstdev(values), (
        statistics.pstdev(values) / mean if mean else 0.0
    )


def validate_pair(control_path, candidate_path):
    control, candidate = read(control_path), read(candidate_path)
    if len(control) != 7 or len(candidate) != 7:
        raise RuntimeError(f"{control_path.stem}: expected seven rows per variant")
    for path, rows in ((control_path, control), (candidate_path, candidate)):
        workers = read(Path(str(path) + ".workers.csv"))
        producers = read(Path(str(path) + ".producers.csv"))
        for row in rows:
            if row["correctness_passed"] != "true":
                raise RuntimeError(f"{path.name}: correctness failure")
            iteration = row["iteration"]
            wr = [item for item in workers if item["iteration"] == iteration]
            pr = [item for item in producers if item["iteration"] == iteration]
            if sum(int(item["tasks_executed"]) for item in wr) != int(row["tasks_executed"]):
                raise RuntimeError(f"{path.name}: worker total mismatch")
            if sum(int(item["tasks_accepted"]) for item in pr) != int(row["tasks_accepted"]):
                raise RuntimeError(f"{path.name}: producer total mismatch")
    return control, candidate


def summarize():
    records = []
    controls = sorted(
        path for path in RAW.glob("control_*.csv")
        if ".workers." not in path.name and ".producers." not in path.name
        and "stress_" not in path.name
    )
    if len(controls) != 14:
        raise RuntimeError(f"expected 14 primary control configurations, found {len(controls)}")
    for control_path in controls:
        scenario = control_path.stem.removeprefix("control_")
        candidate_path = RAW / f"candidate_{scenario}.csv"
        control, candidate = validate_pair(control_path, candidate_path)
        ctp, ntp = med(control, "throughput_tasks_per_second"), med(candidate, "throughput_tasks_per_second")
        cns, nns = med(control, "total_duration_ns"), med(candidate, "total_duration_ns")
        cmin, cmax, csd, ccv = stats(control, "throughput_tasks_per_second")
        nmin, nmax, nsd, ncv = stats(candidate, "throughput_tasks_per_second")
        csig = med(control, "not_empty_signals") + med(control, "not_full_signals")
        nsig = med(candidate, "not_empty_signals") + med(candidate, "not_full_signals")
        sample = control[0]
        records.append({
            "scenario": scenario,
            "callback": sample["callback_profile"],
            "workers": sample["workers"],
            "producers": sample["producers"],
            "capacity": sample["capacity"],
            "task_count": sample["tasks_attempted"],
            "control_median_tasks_per_sec": ctp,
            "candidate_median_tasks_per_sec": ntp,
            "throughput_delta_percent": (ntp / ctp - 1.0) * 100.0,
            "candidate_control_ratio": ntp / ctp,
            "control_throughput_min": cmin,
            "control_throughput_max": cmax,
            "control_throughput_stddev": csd,
            "control_throughput_cv": ccv,
            "candidate_throughput_min": nmin,
            "candidate_throughput_max": nmax,
            "candidate_throughput_stddev": nsd,
            "candidate_throughput_cv": ncv,
            "control_median_total_ns": cns,
            "candidate_median_total_ns": nns,
            "duration_delta_percent": (nns / cns - 1.0) * 100.0,
            "control_median_submit_latency_ns": med(control, "mean_submit_latency_ns"),
            "candidate_median_submit_latency_ns": med(candidate, "mean_submit_latency_ns"),
            "control_p95_submit_latency_ns": med(control, "p95_submit_latency_ns"),
            "candidate_p95_submit_latency_ns": med(candidate, "p95_submit_latency_ns"),
            "control_enqueue_wait_rate": med(control, "enqueue_wait_count") / med(control, "enqueue_lock_attempts"),
            "candidate_enqueue_wait_rate": med(candidate, "enqueue_wait_count") / med(candidate, "enqueue_lock_attempts"),
            "control_dequeue_wait_rate": med(control, "dequeue_wait_count") / med(control, "dequeue_lock_attempts"),
            "candidate_dequeue_wait_rate": med(candidate, "dequeue_wait_count") / med(candidate, "dequeue_lock_attempts"),
            "control_not_empty_signals": med(control, "not_empty_signals"),
            "candidate_not_empty_signals": med(candidate, "not_empty_signals"),
            "control_not_full_signals": med(control, "not_full_signals"),
            "candidate_not_full_signals": med(candidate, "not_full_signals"),
            "control_signals_per_task": csig / float(sample["tasks_attempted"]),
            "candidate_signals_per_task": nsig / float(sample["tasks_attempted"]),
            "total_signal_reduction_percent": (1.0 - nsig / csig) * 100.0,
            "candidate_avoided_not_empty_signals": med(candidate, "avoided_not_empty_signals"),
            "candidate_avoided_not_full_signals": med(candidate, "avoided_not_full_signals"),
            "control_predicate_false_wakeups": med(control, "enqueue_predicate_false_wakeups") + med(control, "dequeue_predicate_false_wakeups"),
            "candidate_predicate_false_wakeups": med(candidate, "enqueue_predicate_false_wakeups") + med(candidate, "dequeue_predicate_false_wakeups"),
            "control_worker_task_cv": med(control, "worker_task_cv"),
            "candidate_worker_task_cv": med(candidate, "worker_task_cv"),
            "control_zero_task_workers": med(control, "zero_task_workers"),
            "candidate_zero_task_workers": med(candidate, "zero_task_workers"),
            "control_correctness_pass_rate": 1.0,
            "candidate_correctness_pass_rate": 1.0,
            "decision": "OPTIMIZATION REJECTED",
        })
    with OUT.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)
    return records


def chart(name, title, labels, control, candidate=None, y_label="value"):
    CHARTS.mkdir(parents=True, exist_ok=True)
    w, h, left, top, pw, ph = 900, 480, 80, 55, 790, 330
    all_values = list(control) + ([] if candidate is None else list(candidate))
    ymax = max(all_values + [1.0]) * 1.12
    group = pw / len(labels)
    bw = group * (0.32 if candidate is not None else 0.58)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
             '<rect width="100%" height="100%" fill="white"/>',
             f'<text x="450" y="28" text-anchor="middle" font-family="sans-serif" font-size="19">{title}</text>',
             f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+ph}" stroke="#333"/>',
             f'<line x1="{left}" y1="{top+ph}" x2="{left+pw}" y2="{top+ph}" stroke="#333"/>']
    for tick in range(6):
        value, y = ymax * tick / 5, top + ph - ph * tick / 5
        parts += [f'<line x1="{left}" y1="{y}" x2="{left+pw}" y2="{y}" stroke="#ddd"/>',
                  f'<text x="{left-8}" y="{y+4}" text-anchor="end" font-family="sans-serif" font-size="11">{value:.3g}</text>']
    for i, label in enumerate(labels):
        center = left + group * (i + .5)
        values = [(control[i], "#2364aa")]
        if candidate is not None:
            values.append((candidate[i], "#d1495b"))
        for j, (value, color) in enumerate(values):
            x = center - bw * len(values) / 2 + j * bw
            y = top + ph - value / ymax * ph
            parts.append(f'<rect x="{x}" y="{y}" width="{bw-2}" height="{top+ph-y}" fill="{color}"/>')
        parts.append(f'<text x="{center}" y="{top+ph+20}" text-anchor="middle" font-family="sans-serif" font-size="10">{label}</text>')
    parts.append(f'<text x="18" y="220" transform="rotate(-90 18 220)" text-anchor="middle" font-family="sans-serif" font-size="12">{y_label}</text>')
    parts.append('<rect x="90" y="435" width="14" height="14" fill="#2364aa"/><text x="110" y="447" font-family="sans-serif" font-size="12">control</text>')
    if candidate is not None:
        parts.append('<rect x="190" y="435" width="14" height="14" fill="#d1495b"/><text x="210" y="447" font-family="sans-serif" font-size="12">candidate</text>')
    parts.append("</svg>")
    (CHARTS / name).write_text("\n".join(parts) + "\n", encoding="utf-8")


def charts(rows):
    by = {row["scenario"]: row for row in rows}
    workers = ["1", "2", "4", "8"]
    noop = [by[f"noop_w{w}_p4_c64"] for w in workers]
    light = [by[f"light_w{w}_p4_c64"] for w in workers]
    labels = [row["scenario"] for row in rows]
    chart("01-noop-worker-throughput.svg", "No-op throughput by workers", workers, [r["control_median_tasks_per_sec"] for r in noop], [r["candidate_median_tasks_per_sec"] for r in noop], "Tasks/s")
    chart("02-light-worker-throughput.svg", "Light CPU throughput by workers", workers, [r["control_median_tasks_per_sec"] for r in light], [r["candidate_median_tasks_per_sec"] for r in light], "Tasks/s")
    chart("03-throughput-delta.svg", "Candidate throughput delta", labels, [r["throughput_delta_percent"] for r in rows], None, "percent")
    chart("04-signals-per-task.svg", "Signal operations per Task", labels, [r["control_signals_per_task"] for r in rows], [r["candidate_signals_per_task"] for r in rows], "signals/Task")
    chart("05-signal-reduction.svg", "Signal reduction", labels, [r["total_signal_reduction_percent"] for r in rows], None, "percent")
    chart("06-enqueue-wait-rate.svg", "Enqueue wait rate", labels, [100*r["control_enqueue_wait_rate"] for r in rows], [100*r["candidate_enqueue_wait_rate"] for r in rows], "percent")
    chart("07-dequeue-wait-rate.svg", "Dequeue wait rate", labels, [100*r["control_dequeue_wait_rate"] for r in rows], [100*r["candidate_dequeue_wait_rate"] for r in rows], "percent")
    chart("08-predicate-false-wakeups.svg", "Predicate-false wake-ups", labels, [r["control_predicate_false_wakeups"] for r in rows], [r["candidate_predicate_false_wakeups"] for r in rows], "wake-ups")
    chart("09-worker-cv.svg", "Worker distribution CV", labels, [r["control_worker_task_cv"] for r in rows], [r["candidate_worker_task_cv"] for r in rows], "CV")
    cap1 = by["noop_w4_p4_c1"]
    chart("10-capacity-one.svg", "Capacity-one throughput", ["capacity 1"], [cap1["control_median_tasks_per_sec"]], [cap1["candidate_median_tasks_per_sec"]], "Tasks/s")
    producers = [by["noop_w4_p1_c64"], by["noop_w4_p2_c64"], by["noop_w4_p4_c64"]]
    chart("11-producer-scaling.svg", "Producer scaling", ["1", "2", "4"], [r["control_median_tasks_per_sec"] for r in producers], [r["candidate_median_tasks_per_sec"] for r in producers], "Tasks/s")
    medium = by["medium_w4_p4_c64"]
    chart("12-medium-safeguard.svg", "Medium CPU safeguard", ["medium"], [medium["control_median_tasks_per_sec"]], [medium["candidate_median_tasks_per_sec"]], "Tasks/s")
    stress_control = [read(RAW / f"control_stress_noop_w8_p4_c64_run{i}.csv")[0] for i in range(1, 4)]
    stress_candidate = [read(RAW / f"candidate_stress_noop_w8_p4_c64_run{i}.csv")[0] for i in range(1, 4)]
    if not all(row["correctness_passed"] == "true" for row in stress_control + stress_candidate):
        raise RuntimeError("stress correctness failed")
    chart("13-stress-correctness.svg", "Million-Task stress correctness", ["run 1", "run 2", "run 3"], [1, 1, 1], [1, 1, 1], "pass=1")


def main():
    rows = summarize()
    charts(rows)
    print(f"validated {len(rows)} paired configurations and six stress runs")


if __name__ == "__main__":
    main()
