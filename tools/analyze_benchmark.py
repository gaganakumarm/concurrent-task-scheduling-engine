#!/usr/bin/env python3
"""Summarize validated scheduler benchmark CSV files with no dependencies."""

import argparse
import csv
import statistics
import sys
from pathlib import Path


GROUP_FIELDS = (
    "scenario",
    "callback_profile",
    "mode",
    "workers",
    "producers",
    "capacity",
    "tasks_attempted",
)


def require(row, field, source):
    value = row.get(field)
    if value is None or value == "":
        raise ValueError(f"{source}: missing {field}")
    return value


def load(paths):
    groups = {}
    for path in paths:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            for line, row in enumerate(reader, 2):
                source = f"{path}:{line}"
                if require(row, "correctness_passed", source) != "true":
                    raise ValueError(f"{source}: correctness did not pass")
                for field in (
                    "snapshot_overflow",
                    "validation_violations",
                    "validation_incomplete",
                ):
                    if field in row and require(row, field, source) != "0":
                        raise ValueError(f"{source}: {field} is not zero")
                if "derived_health" in row:
                    if require(row, "derived_health", source) != "STOPPED":
                        raise ValueError(f"{source}: health is not STOPPED")
                key = tuple(require(row, field, source) for field in GROUP_FIELDS)
                throughput = float(
                    require(row, "throughput_tasks_per_second", source)
                )
                elapsed = int(require(row, "total_duration_ns", source))
                high_water = int(row.get("queue_high_water_mark") or 0)
                groups.setdefault(key, []).append(
                    (throughput, elapsed, high_water)
                )
    if not groups:
        raise ValueError("no benchmark rows found")
    return groups


def summarize(groups):
    rows = []
    for key in sorted(groups):
        samples = groups[key]
        throughput = [sample[0] for sample in samples]
        elapsed = [sample[1] for sample in samples]
        high_water = [sample[2] for sample in samples]
        mean = statistics.fmean(throughput)
        deviation = statistics.pstdev(throughput)
        rows.append(
            dict(
                zip(GROUP_FIELDS, key),
                samples=len(samples),
                throughput_median=statistics.median(throughput),
                throughput_min=min(throughput),
                throughput_max=max(throughput),
                throughput_mean=mean,
                throughput_stddev=deviation,
                throughput_cv_percent=(
                    0.0 if mean == 0.0 else deviation / mean * 100.0
                ),
                elapsed_p50_ns=statistics.median(elapsed),
                elapsed_p95_ns=sorted(elapsed)[
                    max(0, (95 * len(elapsed) + 99) // 100 - 1)
                ],
                queue_high_water_max=max(high_water),
            )
        )
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rows = summarize(load(args.csv))
    fields = list(rows[0])
    stream = args.output.open("w", newline="", encoding="utf-8") \
        if args.output else sys.stdout
    try:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    finally:
        if args.output:
            stream.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"benchmark analysis failed: {error}", file=sys.stderr)
        raise SystemExit(1)
