#!/usr/bin/env python3
"""Exhaustive check of the abstract Exchange / BodyStream model.

This is a DESIGN artifact for docs/EXCHANGE.md, not the implementation: it
explores every interleaving of an abstract model of one Exchange (two bounded
BodyStreams, an ingress side, a responder side) and checks the rules the spec
claims. It has no dependency on the C++ tree.

  usage: tools/exchange_model.py           (exit 1 if any expectation fails)

What is modelled (names match docs/EXCHANGE.md):
  * BodyStream credit: claim / push / release / consume, `want` registration
    and the Writable / Readable wake rules, abort, finish.
  * Exchange coupling: response head publication (early response), request
    cancel by the responder, router-synthesized error head, detach rules.
  * Fault injection: ingress dies, responder dies, responder aborts the
    response body.

What is NOT modelled: bytes' content, trailers (a flag on finish), timers,
the ring, chunk-pool exhaustion, tunnels, H1/H2/H3 framing.

Checks, per configuration:
  SAFETY    queued + claimed <= H on every stream, in every state.
  CLEAN     in every freed state both streams hold no bytes and no credit, and
            each is Aborted or fully drained.
  PROGRESS  (no-fault mode) every state without an enabled action is freed:
            a lost wakeup or an unbreakable wait would show up here.
  REACH     (fault mode) every reachable state can still reach a freed state.
Each "mutation" below removes one rule and MUST make a check fail; this proves
the checker can see the bug class the rule exists to prevent.
"""

import sys
from collections import deque

# ---- layout of the flat state tuple ------------------------------------------
# stream fields
ST, QD, CL, WW, WR, EVW, EVR, PP, PC, PR, PD, CP = range(12)
NF = 12
OPEN, FIN, ABT = 0, 1, 2
# per-stream producer phase PP: 0 idle, 1 blocked (waits for Writable), 2 holds claim
# per-stream consumer phase CP: 0 idle, 1 waiting (waits for Readable), 2 done
# exchange fields (after the two streams)
AI, AR, HEAD, EVH, GH, FREED = range(2 * NF, 2 * NF + 6)
NS = 2 * NF + 6
REQ, RES = 0, 1


class Cfg:
    def __init__(self, H, L, Q, T, faults, mutation=None):
        self.H, self.L, self.Q, self.T = H, L, Q, T
        self.faults = faults
        self.mut = mutation

    def __repr__(self):
        return f"H={self.H} L={self.L} Q={self.Q} T={self.T}"


def init(cfg):
    s = [0] * NS
    for i in (REQ, RES):
        s[i * NF + PR] = cfg.T
    s[AI] = s[AR] = 1
    return tuple(s)


def base(i):
    return i * NF


def is_done_consumed(s, i):
    """consumer has seen the end: aborted, or finished and drained."""
    b = base(i)
    return s[b + ST] == ABT or (s[b + ST] == FIN and s[b + QD] == 0)


def wake_w(cfg, s, i):
    """BodyStream rule: wake a blocked producer once the low watermark is
    reached AND the credit it asked for is available."""
    b = base(i)
    if s[b + WW] > 0 and s[b + ST] == OPEN:
        credit = cfg.H - s[b + QD] - s[b + CL]
        if s[b + QD] <= cfg.L and credit >= s[b + WW]:
            s[b + WW] = 0
            s[b + EVW] = 1


def abort(cfg, s, i, cancel):
    """Terminal: queue discarded, credit dropped, both sides notified."""
    b = base(i)
    if s[b + ST] == ABT:
        return
    s[b + ST] = ABT
    s[b + QD] = 0
    if cfg.mut != "abort_keeps_claim":
        s[b + CL] = 0
    s[b + WW] = 0
    if cfg.mut != "abort_no_wake":
        if s[b + PP] == 1:
            s[b + EVW] = 1
        if s[b + CP] == 1:
            s[b + WR] = 0
            s[b + EVR] = 1


def prod_attached(s, i):
    return s[AI] if i == REQ else s[AR]


def cons_attached(s, i):
    return s[AR] if i == REQ else s[AI]


def detach(cfg, s, side):
    """Detach rule: a producer that has not finished aborts its stream; a
    consumer that has not seen the end cancels it. A producer that already
    finished leaves the stream for the consumer to drain."""
    if side == "i":
        s[AI] = 0
        prod, cons = REQ, RES
    else:
        s[AR] = 0
        prod, cons = RES, REQ
    if cfg.mut == "no_detach_cancel":
        return
    if s[base(prod) + ST] == OPEN:
        abort(cfg, s, prod, cancel=False)
    if not is_done_consumed(s, cons):
        abort(cfg, s, cons, cancel=True)
    # an attached-then-detached agent never acts again; clear its local waits
    if side == "i":
        pass


def succ(cfg, st):
    """All (label, next_state) pairs."""
    out = []
    H = cfg.H

    def nxt(label, fn):
        s = list(st)
        fn(s)
        t = tuple(s)
        if t != st:
            out.append((label, t))

    if st[FREED]:
        return out

    for i in (REQ, RES):
        b = base(i)
        name = "req" if i == REQ else "res"
        pa, ca = prod_attached(st, i), cons_attached(st, i)

        # ---------------- producer ----------------
        if pa and not st[b + PD]:
            head_ok = i == REQ or st[HEAD] != 0
            if st[b + ST] == ABT and st[b + PP] in (0, 2):

                def f(s, b=b):
                    s[b + PP] = 0
                    s[b + PC] = 0
                    s[b + PD] = 1

                nxt(f"{name}.P observes abort", f)
            if st[b + ST] == OPEN and head_ok:
                if st[b + PP] == 0 and st[b + PR] > 0:

                    def f(s, b=b):
                        want = min(cfg.Q, s[b + PR])
                        credit = H - s[b + QD] - s[b + CL]
                        if credit >= want:
                            s[b + CL] += want
                            s[b + PC] = want
                            s[b + PP] = 2
                        else:
                            s[b + WW] = want
                            s[b + PP] = 1

                    nxt(f"{name}.P claim", f)
                if st[b + PP] == 2:
                    ks = {st[b + PC]}
                    if st[b + PC] > 1:
                        ks.add(st[b + PC] - 1)  # short read: release the rest
                    for k in sorted(ks):

                        def f(s, b=b, i=i, k=k):
                            c = s[b + PC]
                            s[b + CL] -= c
                            s[b + QD] += k
                            s[b + PR] -= k
                            s[b + PP] = 0
                            s[b + PC] = 0
                            if s[b + WR]:
                                s[b + WR] = 0
                                s[b + EVR] = 1
                            if k < c and cfg.mut != "no_release_wake":
                                wake_w(cfg, s, i)

                        nxt(f"{name}.P push {k}", f)
                if st[b + PP] == 0 and st[b + PR] == 0:

                    def f(s, b=b):
                        s[b + ST] = FIN
                        s[b + PD] = 1
                        if s[b + WR]:
                            s[b + WR] = 0
                            s[b + EVR] = 1

                    nxt(f"{name}.P finish", f)
                if st[b + PP] == 2:
                    # The read that was granted credit returns EOF / END_STREAM
                    # (0 bytes): finish must give back the claim. (A producer
                    # BLOCKED on credit has not read yet, so it cannot learn of
                    # EOF; allowing that would mask real lost-wakeup bugs.)
                    def f(s, b=b):
                        if cfg.mut != "finish_keeps_claim":
                            s[b + CL] -= s[b + PC]
                            s[b + PC] = 0
                        s[b + WW] = 0
                        s[b + PP] = 0
                        s[b + ST] = FIN
                        s[b + PD] = 1
                        if s[b + WR]:
                            s[b + WR] = 0
                            s[b + EVR] = 1

                    nxt(f"{name}.P finish early", f)
        # event delivery to the producer
        if st[b + EVW]:

            def f(s, b=b, pa=pa):
                s[b + EVW] = 0
                if pa and s[b + PP] == 1:
                    s[b + PP] = 0

            nxt(f"{name}.deliver Writable", f)

        # ---------------- consumer ----------------
        gate = i == REQ or st[GH]
        if ca and st[b + CP] == 0 and gate:
            if st[b + ST] == ABT or (st[b + ST] == FIN and st[b + QD] == 0):

                def f(s, b=b):
                    s[b + CP] = 2

                nxt(f"{name}.C sees end", f)
            elif st[b + QD] > 0:
                for k in range(1, st[b + QD] + 1):

                    def f(s, b=b, i=i, k=k):
                        s[b + QD] -= k
                        if cfg.mut != "no_consume_wake":
                            wake_w(cfg, s, i)

                    nxt(f"{name}.C consume {k}", f)
            else:  # empty and still open: register interest, then wait

                def f(s, b=b):
                    s[b + WR] = 1
                    s[b + CP] = 1

                nxt(f"{name}.C wait", f)
        if st[b + EVR]:

            def f(s, b=b, ca=ca):
                s[b + EVR] = 0
                if ca and s[b + CP] == 1:
                    s[b + CP] = 0

            nxt(f"{name}.deliver Readable", f)

    # ---------------- responder ----------------
    if st[AR]:
        if st[HEAD] == 0:

            def f(s):
                s[HEAD] = 1
                s[EVH] = 1

            nxt("resp publish head", f)
        # early response: after the head the responder may stop reading the request
        if st[HEAD] != 0 and not is_done_consumed(st, REQ) and st[base(REQ) + CP] != 2:

            def f(s):
                abort(cfg, s, REQ, cancel=True)

            nxt("resp cancel request", f)
        # normal detach: its response is finished or aborted
        if st[base(RES) + PD] and st[HEAD] != 0:

            def f(s):
                detach(cfg, s, "r")

            nxt("resp detach", f)
    if st[EVH]:

        def f(s):
            s[EVH] = 0
            if s[AI]:
                s[GH] = 1

        nxt("deliver head", f)

    # ---------------- ingress detach ----------------
    if st[AI] and st[base(RES) + CP] == 2 and st[base(REQ) + PD]:

        def f(s):
            detach(cfg, s, "i")

        nxt("ingress detach", f)

    # ---------------- faults ----------------
    if cfg.faults:
        if st[AI]:

            def f(s):
                detach(cfg, s, "i")

            nxt("FAULT ingress dies", f)
        if st[AR]:

            def f(s):
                if s[HEAD] == 0 and s[AI]:
                    # router synthesizes an error response: head + empty body
                    s[HEAD] = 2
                    s[EVH] = 1
                    b = base(RES)
                    s[b + ST] = FIN
                    s[b + PR] = 0
                    s[b + PD] = 1
                detach(cfg, s, "r")

            nxt("FAULT responder dies", f)
            if st[HEAD] != 0 and st[base(RES) + ST] == OPEN:

                def f(s):
                    abort(cfg, s, RES, cancel=False)

                nxt("FAULT responder aborts response", f)

    # ---------------- free ----------------
    if not st[AI] and not st[AR]:

        def f(s):
            s[FREED] = 1

        nxt("FREE", f)
    return out


def check_state(cfg, st, errs):
    for i in (REQ, RES):
        b = base(i)
        if st[b + QD] + st[b + CL] > cfg.H:
            errs.append(("SAFETY: queued+claimed > H", st))
    for i in (REQ, RES):
        b = base(i)
        # LOSTWAKE: an agent parked on a wait that can no longer be satisfied
        # and has no wakeup in flight. In production only a deadline (I9)
        # would rescue it; the model must call it a bug.
        if (prod_attached(st, i) and st[b + PP] == 1 and st[b + ST] == ABT
                and not st[b + EVW]):
            errs.append(("LOSTWAKE: producer blocked on an aborted stream", st))
        if (cons_attached(st, i) and st[b + CP] == 1 and st[b + ST] != OPEN
                and not st[b + EVR]):
            errs.append(("LOSTWAKE: consumer waiting on a finished/aborted stream", st))
    if st[FREED]:
        for i in (REQ, RES):
            b = base(i)
            if st[b + QD] != 0 or st[b + CL] != 0:
                errs.append(("CLEAN: bytes/credit left at free", st))
            if not (st[b + ST] == ABT or (st[b + ST] == FIN and st[b + QD] == 0)):
                errs.append(("CLEAN: stream neither aborted nor drained at free", st))


def explore(cfg, limit=3_000_000):
    start = init(cfg)
    seen = {start}
    edges = {}
    q = deque([start])
    errs = []
    stuck = []
    cov = {"blocked": 0, "abort_while_blocked": 0, "consumer_waiting": 0}
    while q:
        s = q.popleft()
        check_state(cfg, s, errs)
        for i in (REQ, RES):
            b = base(i)
            if s[b + PP] == 1 and s[b + ST] == OPEN:
                cov["blocked"] += 1
            if s[b + PP] == 1 and s[b + ST] == ABT:
                cov["abort_while_blocked"] += 1
            if s[b + CP] == 1:
                cov["consumer_waiting"] += 1
        ns = succ(cfg, s)
        edges[s] = [t for _, t in ns]
        if not ns and not s[FREED]:
            stuck.append(s)
        for _, t in ns:
            if t not in seen:
                seen.add(t)
                q.append(t)
                if len(seen) > limit:
                    raise RuntimeError("state space too large")
    unreach = []
    if cfg.faults:
        # REACH: backward closure from freed states must cover everything.
        rev = {}
        for s, ts in edges.items():
            for t in ts:
                rev.setdefault(t, []).append(s)
        good = {s for s in edges if s[FREED]}
        dq = deque(good)
        while dq:
            t = dq.popleft()
            for p in rev.get(t, ()):
                if p not in good:
                    good.add(p)
                    dq.append(p)
        unreach = [s for s in edges if s not in good]
    return len(seen), errs, stuck, unreach, cov


def verdict(cfg):
    n, errs, stuck, unreach, cov = explore(cfg)
    bad = []
    # COVERAGE: a pass is only meaningful if the paths it is about were reached.
    vac = [k for k in ("blocked", "consumer_waiting") if cov[k] == 0]
    if cfg.faults and cov["abort_while_blocked"] == 0:
        vac.append("abort_while_blocked")
    if vac:
        bad.append("VACUOUS (never reached: " + ", ".join(vac) + ")")
    if errs:
        bad.append(f"{len(errs)} invariant violation(s): {errs[0][0]}")
    if stuck:
        bad.append(f"{len(stuck)} stuck state(s) with no enabled action")
    if unreach:
        bad.append(f"{len(unreach)} state(s) cannot reach free")
    return n, bad, cov


def main():
    # (H, L, Q, T): H-L >= Q is the configuration rule of docs/EXCHANGE.md.
    good = [(3, 1, 2, 5), (4, 1, 2, 6), (4, 2, 2, 6), (2, 0, 2, 4), (5, 2, 3, 7)]
    rows = []
    failures = 0

    def run(label, cfg, expect_ok):
        nonlocal failures
        n, bad, cov = verdict(cfg)
        ok = (not bad) == expect_ok
        failures += 0 if ok else 1
        mark = "ok  " if ok else "FAIL"
        what = "clean" if not bad else "; ".join(bad)
        exp = "expect clean" if expect_ok else "expect violation"
        rows.append(f"[{mark}] {label:<30} {cfg!s:<18} states={n:<7} "
                    f"blocked={cov['blocked']:<6} {exp}: {what}")

    for H, L, Q, T in good:
        run("no faults", Cfg(H, L, Q, T, False), True)
        run("with faults", Cfg(H, L, Q, T, True), True)

    # Findings that shaped the spec (see docs/EXCHANGE.md section 4.6):
    #  * H-L < Q is NOT a liveness problem: the wake needs queued<=L AND enough
    #    credit, so the effective resume level is min(L, H-Q). Efficiency only.
    run("H-L < Q: clean, resume=min(L,H-Q)", Cfg(3, 2, 2, 5, False), True)
    #  * Q > H IS a liveness problem: a claim larger than H is never grantable.
    #    The spec therefore clamps every claim to min(Q, H).
    run("Q > H: unclamped claim deadlocks", Cfg(1, 0, 2, 4, False), False)

    # Mutations: remove one rule, the checker must notice.
    m = (4, 1, 2, 6)
    run("mutation: no wake on consume", Cfg(*m, False, "no_consume_wake"), False)
    # Single producer per stream: a blocked producer holds no claim, so a
    # release can never be what unblocks it. The rule is REDUNDANT, so the
    # spec does not require it (clean is the correct, informative outcome).
    run("redundant: no wake on release", Cfg(*m, False, "no_release_wake"), True)
    run("mutation: abort does not wake", Cfg(*m, True, "abort_no_wake"), False)
    run("mutation: abort keeps credit", Cfg(*m, True, "abort_keeps_claim"), False)
    run("mutation: finish keeps credit", Cfg(*m, False, "finish_keeps_claim"), False)
    run("mutation: detach does not cancel", Cfg(*m, True, "no_detach_cancel"), False)

    print("\n".join(rows))
    print(f"\n{failures} unexpected result(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
