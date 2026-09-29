#!/usr/bin/env python3
"""
Quick console health check for the local GPUs (NVML, nvtop-style).
Polls every GPU plus a per-process VRAM breakdown and prints one screenful
per interval. Run this ON the machine with the GPUs.

Run with: .venv/bin/python gpu_monitor.py
"""

import time
from datetime import datetime

from gpu_stats_server import NvmlSampler, POLL_INTERVAL


def _fmt_gb(v):
    return f"{v:.1f}GB" if v is not None else "n/a"


def main():
    sampler = NvmlSampler()
    print(f"NVML sees {len(sampler.handles)} GPU(s). Polling every {POLL_INTERVAL}s. Ctrl+C to stop.\n")
    try:
        while True:
            snap = sampler.snapshot()
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] activity={snap['activity']}")
            for g in snap["gpus"]:
                util = f"{g['util_pct']:.0f}%" if g["util_pct"] is not None else "n/a"
                temp = f"{g['temp_c']}C" if g["temp_c"] is not None else "n/a"
                power = f"{g['power_w']}W" if g["power_w"] is not None else "n/a"
                vram = f"{_fmt_gb(g['vram_gb'])}/{_fmt_gb(g['vram_total_gb'])}"
                print(f"  GPU{g['index']} {g['name']:<32} util {util:>4} | vram {vram:>14} | {temp} {power}")
            if snap["processes"]:
                plist = "  ".join(
                    f"pid{p['pid']} {p['name']} {p['vram_mb']/1024:.1f}GB(gpu{p['gpu']})"
                    if p["vram_mb"] >= 1024
                    else f"pid{p['pid']} {p['name']} {p['vram_mb']:.0f}MB(gpu{p['gpu']})"
                    for p in snap["processes"]
                )
                print(f"  procs: {plist}")
            else:
                print("  procs: (none)")
            print()
            time.sleep(POLL_INTERVAL)
    finally:
        sampler.shutdown()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
