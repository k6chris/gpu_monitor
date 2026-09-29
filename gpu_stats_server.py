#!/usr/bin/env python3
"""
GPU stats server for the CYD dashboard - Ollama-free.
Polls NVML (the same interface nvtop uses) for every GPU every POLL_INTERVAL
seconds and serves the latest snapshot as JSON on GET /stats. The ESP32
only ever makes one HTTP request per refresh.

Run with: python3 gpu_stats_server.py
Requires: pip install flask nvidia-ml-py
"""

import json
import time
import threading
import urllib.request
from datetime import datetime, timezone

import pynvml
from flask import Flask, jsonify

POLL_INTERVAL = 3   # seconds between background samples
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8090  # CYD hits http://<host-ip>:8090/stats
BUSY_THRESHOLD_PCT = 15.0  # above this = actively computing; below = idle chip noise
LLAMA_BASE_URL = "http://127.0.0.1:8080"  # local llama-server
LLAMA_TIMEOUT = 2    # seconds; /slots must answer fast or we report llama_ok: false

app = Flask(__name__)
_lock = threading.Lock()
_latest = {
    "ok": False,
    "activity": "unreachable",   # "busy" | "idle" | "unreachable"
    "gpu_count": 0,
    "gpus": [],                  # per-GPU nvtop-style metrics
    "processes": [],             # per-(GPU,pid): name, vram_mb
    # Top-level scalars kept for backwards compat with the CYD code:
    "gpu_pct": None,             # highest util across all GPUs
    "vram_gb": None,             # VRAM used, summed across all GPUs
    "vram_pct": None,            # VRAM used %, summed across all GPUs
    # llama-server context window usage (hottest slot):
    "llama_ok": False,           # /slots reachable
    "llama_busy": False,         # any slot is_processing
    "ctx_used": 0,               # tokens in use (prompt + generated so far)
    "ctx_total": 0,              # n_ctx
    "ctx_pct": 0.0,              # ctx_used / ctx_total * 100
    "tok_s": 0.0,                # generation speed, smoothed
    "updated_at": None,
}


def _gb(nbytes):
    return round(nbytes / (1024 ** 3), 2)


def _safe(fn, *args):
    """Run an NVML call; return None on NVA/NOSUPPORT instead of raising."""
    try:
        return fn(*args)
    except Exception:
        return None


def _proc_name(pid):
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return None


class NvmlSampler:
    """Owns the NVML init/shutdown and takes full per-GPU + per-process snapshots."""

    def __init__(self):
        self.handles = []
        try:
            pynvml.nvmlInit()
            count = pynvml.nvmlDeviceGetCount()
            self.handles = [pynvml.nvmlDeviceGetHandleByIndex(i) for i in range(count)]
        except pynvml.NVMLError:
            self.handles = []

    def shutdown(self):
        try:
            pynvml.nvmlShutdown()
        except Exception:
            pass

    def snapshot(self):
        if not self.handles:
            return {
                "ok": False, "activity": "unreachable", "gpu_count": 0,
                "gpus": [], "processes": [],
                "gpu_pct": None, "vram_gb": None, "vram_pct": None,
                "updated_at": datetime.now(timezone.utc).isoformat(),
            }
        gpus, processes = self._snapshot_gpus_and_processes()
        utils = [g["util_pct"] for g in gpus if g["util_pct"] is not None]
        used = sum(g["vram_gb"] for g in gpus if g["vram_gb"] is not None)
        total = sum(g["vram_total_gb"] for g in gpus if g["vram_total_gb"] is not None)
        return {
            "ok": True,
            "activity": "busy" if (utils and max(utils) > BUSY_THRESHOLD_PCT) else "idle",
            "gpu_count": len(gpus),
            "gpus": gpus,
            "processes": processes,
            "gpu_pct": max(utils) if utils else None,
            "vram_gb": round(used, 2),
            "vram_pct": round(100 * used / total, 1) if total else None,
            "updated_at": datetime.now(timezone.utc).isoformat(),
        }

    def _snapshot_gpus_and_processes(self):
        gpus = []
        procs = {}
        for i, handle in enumerate(self.handles):
            mem = _safe(pynvml.nvmlDeviceGetMemoryInfo, handle)
            util = _safe(pynvml.nvmlDeviceGetUtilizationRates, handle)
            power_mw = _safe(pynvml.nvmlDeviceGetPowerUsage, handle)
            power_limit_mw = _safe(pynvml.nvmlDeviceGetPowerManagementLimit, handle)
            sm_mhz = _safe(pynvml.nvmlDeviceGetClockInfo, handle, pynvml.NVML_CLOCK_SM)
            mem_mhz = _safe(pynvml.nvmlDeviceGetClockInfo, handle, pynvml.NVML_CLOCK_MEM)
            gpus.append({
                "index": i,
                "name": pynvml.nvmlDeviceGetName(handle),
                "util_pct": float(util.gpu) if util else None,
                "mem_bw_pct": float(util.memory) if util else None,
                "vram_gb": _gb(mem.used) if mem else None,
                "vram_total_gb": _gb(mem.total) if mem else None,
                "vram_pct": round(100 * mem.used / mem.total, 1) if mem and mem.total else None,
                "temp_c": _safe(pynvml.nvmlDeviceGetTemperature, handle, pynvml.NVML_TEMPERATURE_GPU),
                "power_w": round(power_mw / 1000, 1) if power_mw is not None else None,
                "power_limit_w": round(power_limit_mw / 1000, 1) if power_limit_mw is not None else None,
                "sm_clock_mhz": sm_mhz if sm_mhz else None,
                "mem_clock_mhz": mem_mhz if mem_mhz else None,
                "fan_pct": _safe(pynvml.nvmlDeviceGetFanSpeed, handle),
            })
            for query in (pynvml.nvmlDeviceGetComputeRunningProcesses,
                          pynvml.nvmlDeviceGetGraphicsRunningProcesses):
                for p in _safe(query, handle) or []:
                    key = (i, p.pid)
                    entry = procs.setdefault(key, {"gpu": i, "pid": int(p.pid), "vram_mb": 0.0})
                    entry["name"] = _proc_name(p.pid)
                    entry["vram_mb"] = max(entry["vram_mb"], p.usedGpuMemory / (1024 ** 2))
        processes = sorted(procs.values(), key=lambda e: -e["vram_mb"])
        for e in processes:
            e["vram_mb"] = round(e["vram_mb"], 1)
        return gpus, processes


class LlamaSampler:
    """Polls llama-server's /slots for context window usage.

    This llama.cpp build does not expose n_past, so context used is
    approximated as n_prompt_tokens + n_decoded (generated so far).
    When a slot is idle, /slots keeps reporting the last request's prompt
    size, so the gauge freezes at the last run's peak rather than dropping
    to zero.
    """

    def __init__(self, base_url=LLAMA_BASE_URL, timeout=LLAMA_TIMEOUT):
        self.base_url = base_url
        self.timeout = timeout
        self._prev = None   # (slot_id, used, monotonic ts) of last poll
        self._tok_s = None  # smoothed generation speed

    def snapshot(self):
        out = {"llama_ok": False, "llama_busy": False, "ctx_used": 0,
               "ctx_total": 0, "ctx_pct": 0.0, "tok_s": 0.0}
        try:
            with urllib.request.urlopen(f"{self.base_url}/slots", timeout=self.timeout) as r:
                slots = json.load(r)
        except Exception:
            self._prev = None
            self._tok_s = None
            return out

        busy = False
        best_id, best_used, best_total = None, 0, 0
        for s in slots:
            if s.get("is_processing"):
                busy = True
            nt = (s.get("next_token") or [{}])[0]
            used = int(s.get("n_prompt_tokens") or 0) + int(nt.get("n_decoded") or 0)
            total = int(s.get("n_ctx") or 0)
            if used > best_used:
                best_id, best_used, best_total = s.get("id"), used, total

        now = time.monotonic()
        prev = self._prev
        if not busy or prev is None or prev[0] != best_id or now <= prev[2]:
            self._tok_s = None
        else:
            inst = (best_used - prev[1]) / (now - prev[2])
            if 0 <= inst <= 1000:  # ignore prompt-load jumps / new-request resets
                self._tok_s = inst if self._tok_s is None else 0.7 * self._tok_s + 0.3 * inst
            else:
                self._tok_s = None
        self._prev = (best_id, best_used, now)

        out.update({
            "llama_ok": True,
            "llama_busy": busy,
            "ctx_used": best_used,
            "ctx_total": best_total,
            "ctx_pct": round(100.0 * best_used / best_total, 1) if best_total else 0.0,
            "tok_s": round(self._tok_s, 1) if self._tok_s else 0.0,
        })
        return out


def poll_loop(sampler, llama):
    while True:
        try:
            snap = sampler.snapshot()
        except Exception:
            snap = {"ok": False, "activity": "unreachable"}
        snap.update(llama.snapshot())
        with _lock:
            _latest.update(snap)
        time.sleep(POLL_INTERVAL)


@app.route("/stats")
def stats():
    with _lock:
        return jsonify(_latest)


if __name__ == "__main__":
    sampler = NvmlSampler()
    llama = LlamaSampler()
    threading.Thread(target=poll_loop, args=(sampler, llama), daemon=True).start()
    app.run(host=LISTEN_HOST, port=LISTEN_PORT)
