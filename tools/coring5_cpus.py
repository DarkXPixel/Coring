#!/usr/bin/env python3
"""Print [runtime] worker and CPU settings for the current Linux affinity mask."""

import argparse
import os
from pathlib import Path


def cpu_ids(cpu_list):
    for part in cpu_list.strip().split(","):
        bounds = part.split("-")
        yield from range(int(bounds[0]), int(bounds[-1]) + 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workers", type=int, help="number of distinct CPUs to select")
    parser.add_argument("--node", type=int, help="restrict selection to this NUMA node")
    args = parser.parse_args()
    # The daemon accepts CPU IDs in 0..4095 and up to 1024 workers.
    allowed = {cpu for cpu in os.sched_getaffinity(0) if cpu <= 4095}
    sysfs = Path("/sys/devices/system/node")
    node_files = sorted(
        sysfs.glob("node[0-9]*/cpulist"),
        key=lambda path: int(path.parent.name[4:]),
    )
    if args.node is not None:
        node_files = [
            path for path in node_files if path.parent.name == f"node{args.node}"
        ]
        if not node_files:
            parser.error(f"NUMA node {args.node} is unavailable in sysfs")
    cpus = []
    for path in node_files:
        cpus.extend(sorted(allowed.intersection(cpu_ids(path.read_text()))))
    if args.node is None:
        cpus.extend(sorted(allowed.difference(cpus)))
    if not 1 <= args.workers <= min(1024, len(cpus)):
        parser.error(f"requested {args.workers} workers, found {len(cpus)} eligible CPUs")
    print(f"workers = {args.workers}")
    print(f"cpus = {cpus[:args.workers]}")


if __name__ == "__main__":
    main()
