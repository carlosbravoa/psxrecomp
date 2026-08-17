#!/usr/bin/env python3
"""bugreport_threads.py — read the thread / scheduler trail of an F9 bundle.

    python3 tools/bugreport_threads.py saves/bugreports/<stamp>_hotkey [--last N]

Prints, from report.json: the scheduler escape ring (every ChangeThread /
RFE-yield / resume-at, and the SAFETY-NET resumes = a thread's top-level
dispatch returned pc==0 and control fell back to its yielder), the thread
event trace with kind names, the IRQ contexts, CD / IRQ state and the hot PCs
— newest last — so a "went black but keeps running" report can be read without
the debug server. Also flags the first frame where the escape cadence breaks
(a thread that used to alternate every frame stops appearing).
"""
import argparse
import collections
import json
import sys
from pathlib import Path

KINDS = {1: "save", 2: "restore", 3: "change_enter", 4: "invalid", 5: "same", 6: "inactive_current",
         7: "target_missing", 8: "switch_to", 9: "switch_back", 10: "fiber_entry", 11: "fiber_done",
         12: "fiber_return_restore", 13: "fiber_dispatch_exit", 20: "syscall3_enter",
         24: "syscall3_enter_in_exc", 26: "fiber_dispatch_exit_in_exc", 30: "inexc_switch_escape",
         31: "inexc_switch_defer", 32: "deferred_switch_escape", 33: "deferred_switch_stale",
         40: "SCHED_SAFETY_NET_RESUME"}
REASONS = {0: "continue", 1: "yield_to_tcb", 2: "resume_current", 3: "guest_exit",
           4: "return_to_lobby", 5: "fatal", 100: "SAFETY_NET_RESUME"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bundle")
    ap.add_argument("--last", type=int, default=60, help="escapes / events to print (newest)")
    a = ap.parse_args()
    rep = json.load(open(Path(a.bundle) / "report.json"))
    rt = rep.get("runtime", {})
    host = rep.get("host", {})
    print(f"bundle {a.bundle}: frame {rt.get('ping', {}).get('frame')}, renderer {host.get('renderer')}, build {host.get('build')}")
    esc = rt.get("sched_escape_ring", {})
    ents = esc.get("entries", [])
    print(f"\n== scheduler escapes: total {esc.get('total')}, safety-net resumes {esc.get('safety_net_resumes')} (last at frame {esc.get('safety_net_last_frame')}), {len(ents)} in ring")
    # cadence: per frame, which TCBs appeared as targets
    by_frame = collections.OrderedDict()
    for e in ents:
        by_frame.setdefault(e["frame"], []).append(e)
    tcbs = collections.Counter(e["target"] for e in ents)
    print("   targets seen:", dict(tcbs))
    if len(tcbs) >= 2:
        regular = [t for t, n in tcbs.items() if n >= max(tcbs.values()) // 2]
        missing_since = None
        frames = list(by_frame)
        for f in frames:
            tg = {e["target"] for e in by_frame[f]}
            if any(t not in tg for t in regular):
                if missing_since is None:
                    missing_since = f
            else:
                missing_since = None
        if missing_since is not None:
            print(f"   !! cadence break: from frame {missing_since} on, not every regular thread is switched to each frame")
    for e in ents[-a.last:]:
        print(f"   seq {e['seq']:>7} f{e['frame']:>7} {REASONS.get(e['reason'], e['reason']):<18} cur {e['cur']} -> {e['target']} resume {e['resume_pc']} pc {e['pc']} ra {e['ra']} sp {e['sp']}")
    tt = rt.get("thread_trace", {})
    tents = tt.get("entries", [])
    print(f"\n== thread events: {len(tents)} in trace")
    kinds = collections.Counter(KINDS.get(e.get("kind"), e.get("kind")) for e in tents)
    print("   kinds:", dict(kinds))
    odd = [e for e in tents if e.get("kind") not in (1, 2, 3, 5, 8, 20)]
    if odd:
        print(f"   !! {len(odd)} non-routine events (newest last):")
        for e in odd[-20:]:
            print("     ", {**{"kind": KINDS.get(e.get("kind"), e.get("kind"))}, **{k: v for k, v in e.items() if k not in ("kind",)}})
    for e in tents[-a.last:]:
        d = {k: v for k, v in e.items() if k != "kind"}
        print("   ", KINDS.get(e.get("kind"), e.get("kind")), d)
    for key in ("irq_state", "cdrom_state", "cycles_to_next_event", "get_registers"):
        v = rt.get(key)
        if v:
            print(f"\n== {key}: {json.dumps(v)[:600]}")
    ph = rt.get("phase_hot_static", {})
    if ph:
        print("\n== hot static PCs:", [(t["addr"], t["samples"]) for t in ph.get("top", [])[:10]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
