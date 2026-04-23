# Lunar — Deep Evaluation of the Synchronization and Adjustment Mechanism

**Scope.** This document evaluates, but does not change, Lunar's time-sync pipeline (NTP/NTS query → concurrence gate → PLL/slew clockwork → persisted discipline) from three angles: **security**, **guaranteed correctness**, and **availability**. It closes with a prioritized proposal for improvement.

**Reader aid.** Small-print grey boxes interspersed between sections explain abbreviations and background concepts. They can be skipped by readers already familiar with NTP, NTS, PLLs, and Windows timing primitives.

---

<details>
<summary><sub><i>Primer 0 — orientation: what problem is this clock solving?</i></sub></summary>

<sub>Lunar is an analog desktop clock that refuses to display the Windows system clock. The Windows clock can be wrong (user edit, malware, CMOS battery, drift, broken `w32time`, VM sleep/wake). Lunar reads the true time directly from Internet time servers, keeps its own monotonic running clock locally, and renders a red **INOP** ("inoperative") state whenever it cannot verify the time. The sync-and-adjustment mechanism analysed below is the safety-critical core: it is what lets the clock claim "trusted time" and what lets it refuse to lie.</i></sub>

</details>

---

## 1. Current design (as built)

**Sources, per cycle (fired in parallel):**

- Slot 0 — **NIST** (time.nist.gov, USA) — plain SNTPv4 over UDP/123, no authentication
- Slot 1 — **PTB** (ptbtime1.ptb.de, Germany) — same
- Slot 2 — **NICT** (ntp.nict.jp, Japan) — same
- Slot 3 — **NTS** (rotating) — TLS-1.3 + SPKI-pinned key-establishment, then authenticated SNTP; provider picked randomly per cycle from a small pool

<details>
<summary><sub><i>Primer 1 — NTP, SNTP, NTS, SPKI, KE</i></sub></summary>

<sub><b>NTP</b> (Network Time Protocol, RFC 5905). Standard protocol for distributing time across the Internet. A client timestamps its outgoing request (T1), the server timestamps receipt (T2) and reply (T3), the client timestamps arrival (T4). Round-trip and offset follow from these four numbers.</sub>

<sub><b>SNTP</b> (Simple NTP, RFC 4330). A stateless subset of NTP: one request, one reply, no long-running association, no statistical filtering by the client. Adequate for one-shot polling, which is what Lunar does.</sub>

<sub><b>NTS</b> (Network Time Security, RFC 8915). Cryptographic wrapper around NTP. A one-time <b>Key Establishment</b> (KE) handshake over TLS 1.3 gives the client cookies and keys; subsequent UDP time packets are authenticated with AEAD (<i>Authenticated Encryption with Associated Data</i>) so an on-path attacker cannot forge or tamper with them.</sub>

<sub><b>SPKI pinning</b> (Subject Public Key Info). Instead of trusting the full web-PKI certificate chain, we hardcode the expected hash of the server's public key. Catches compromised CAs (Certificate Authorities) and mis-issuance.</sub>

<sub><b>UDP port 123</b> is the standard NTP port; firewalls and captive portals frequently block or rewrite it.</sub>

</details>

**Trust gate.** NTS must succeed, AND ≥ 2 of 3 core sources must project to within ±200 ms of the NTS anchor on a common QPC moment. Otherwise `TRUST_INOP` — `Clock_NowUtcMs()` refuses to return a time and the UI renders red INOP.

**Anchor.** On an `OK` cycle: `(qpcAtT4, ntpUtcMs)` taken verbatim from the NTS sample.

**Rate discipline.** Per-cycle observed ppm over the inter-sync interval, clamped to ±500 ppm (single-cycle target up to ±2000 ppm before clamp), EMA α = 0.25, persisted to `%APPDATA%\Lunar\discipline.dat` at shutdown, reloaded as bootstrap, rejected at > 30 days against *disciplined* UTC.

**Residual handling.** |residual| ≤ 2 s → slew linearly over 60 s; > 2 s → instant snap (anchor rebase).

**Cadence.** 60 s on `TRUST_OK`, 5 s on `TRUST_INOP`.

**Safeguards.** SNTP response header validation (LI / VN / mode / stratum 1–15); cross-check that a claimed-OK cycle matches the running projection within 200 ms — else "local-oscillator fault" → INOP.

<details>
<summary><sub><i>Primer 2 — QPC, UTC, PLL, EMA, ppm, anchor, slew, snap</i></sub></summary>

<sub><b>QPC</b> (<i>QueryPerformanceCounter</i>). Windows' monotonic hardware tick counter. It never moves backwards, it does not change if the user edits the wall clock, it runs at a fixed frequency (typically 10 MHz). Lunar uses QPC for <i>all</i> local timing and only ever uses server replies for <i>UTC</i>.</sub>

<sub><b>UTC</b> (Coordinated Universal Time). The time standard NTP delivers, in milliseconds since the Unix epoch in Lunar's representation.</sub>

<sub><b>PLL</b> (Phase-Locked Loop). A feedback mechanism that steers a local oscillator to match a reference. "Phase" here is the clock's instantaneous time; "frequency" is its rate. Lunar's PLL runs once per cycle and adjusts the local rate (ppm) so future elapsed QPC ticks project onto correct UTC.</sub>

<sub><b>EMA</b> (<i>Exponential Moving Average</i>). Each new measurement updates the running estimate by a fraction α: <code>new = old + α·(sample − old)</code>. Lunar uses α = 0.25 for the rate estimate. Lower α → more damping, slower response; higher α → noisier, faster.</sub>

<sub><b>ppm</b> (parts per million). A fractional rate offset. +57 ppm means the local crystal runs 57 µs fast per second, which is 4.9 s/day. Typical quartz drift is ±50 ppm.</sub>

<sub><b>Anchor</b>. A pair <code>(qpc, utc)</code> that pins "this QPC moment corresponded to this UTC". Projecting forward uses <code>utc + (qpc_now − qpc_anchor) · (1 + ppm/1e6)</code>.</sub>

<sub><b>Slew</b> vs <b>snap</b>. <i>Slew</i> = apply a small correction gradually over a window so the second hand stays smooth. <i>Snap</i> = rebase the anchor instantly, which makes the clock jump but is the only sane option for large errors (sleep/wake, first good sync after a bad bootstrap).</sub>

<sub><b>INOP</b> (inoperative). Aviation-inspired term: when the instrument cannot guarantee correct data, it says so instead of guessing. Lunar renders a red "INOP" state rather than display an untrusted time.</sub>

</details>

---

## 2. Security analysis

### Threat model (implicit today)

Protected against: passive on-path observer, single-ISP hijack of UDP/123, DNS poisoning of *one* source, rate-limiting or outage of *one* source. Relies on NTS as the cryptographic trust anchor.

<details>
<summary><sub><i>Primer 3 — trust anchor, threat model, on-path vs off-path attacker</i></sub></summary>

<sub><b>Trust anchor</b>. The single element whose integrity, if preserved, guarantees the integrity of the whole verdict. Here: the NTS reply. Even if all three plaintext SNTP sources were fully controlled by the adversary, disagreement with the NTS reading would trip INOP.</sub>

<sub><b>On-path attacker</b>. Sits between client and server, can read, drop, modify, inject traffic (e.g. hostile ISP, malicious Wi-Fi, state-level filter). <b>Off-path attacker</b>. Can only inject, not observe; needs to guess response fields (harder). NTS defeats both for the time payload; plain SNTP defeats neither.</sub>

<sub><b>Threat model</b>. An explicit enumeration of which adversaries, capabilities, and attack goals the design intends to defeat. "Implicit today" means Lunar does not have a written one; it is inferred from the code.</sub>

</details>

### Weaknesses

**S1. NTS is a single point of cryptographic trust — silently.**
If the NTS slot fails for any reason (KE timeout, pin mismatch, provider outage), the gate goes INOP. Correct fail-safe, but an adversary who can selectively block TLS to *all* NTS providers while letting UDP/123 through keeps the clock indefinitely INOP — a **denial-of-availability attack via the security layer**. There is no "NTS-disabled, degraded-trust fallback" — INOP is permanent until NTS recovers.

**S2. Core sources use hardcoded hostnames, no DNSSEC.**
A resolver-level attacker can return any IP. They can't forge agreement with NTS (defeated at the concurrence gate) but they *can* deny a core source (drop / RST) or flood with garbage to consume the slot's socket budget. Combined with mild NTS degradation, this widens the DoS attack surface.

<details>
<summary><sub><i>Primer 4 — DNS, DNSSEC, RST, rate-limiting</i></sub></summary>

<sub><b>DNS</b> (Domain Name System). Translates "time.nist.gov" into an IP address. Responses are normally unauthenticated; a hostile resolver can point a name at any address.</sub>

<sub><b>DNSSEC</b>. Cryptographic signing of DNS responses. Windows' default resolver does not validate DNSSEC end-to-end; most apps inherit that. Lunar currently does no DNSSEC validation of its own.</sub>

<sub><b>TCP RST</b>. A "reset" packet that forcibly closes a connection. An attacker can inject RSTs to break TLS handshakes selectively.</sub>

<sub><b>Rate-limiting</b>. Public time servers throttle or ban clients that poll too often. Relevant to A3 below.</sub>

</details>

**S3. NTS provider pool rotation is random, not load-shed-aware.**
If one pinned provider serves stale or bad data (compromised or misconfigured), it gets picked ~1/N of cycles. There is no penalty or blacklist for a pin that recently produced an outlier tripping the local-oscillator fault check. One bad provider keeps cycling in.

**S4. The 200 ms local-oscillator fault check is symmetric with the concurrence threshold.**
This is elegant — but it means an attacker who can induce a ~150 ms NTS offset (within gate) that is *consistent* with two hijacked core sources will succeed. The gate is "≥ 2 core agree with NTS within 200 ms", not "…within a **tighter** bound that would require defeating cryptography". Because NTS is authenticated, in practice the threat is provider compromise; but the gate architecture does not distinguish "all four sources agree to 20 ms" (high confidence) from "scraped agreement at 199 ms" (suspicious).

**S5. Persisted rate file is unauthenticated.**
`%APPDATA%\Lunar\discipline.dat` is plain ASCII `"<ppm> <lastSyncUtcMs>\n"`. A local attacker (or ransomware) that can write `AppData` can inject ±500 ppm which will be applied as bootstrap until the first sync re-verifies it. Window: up to ~6 seconds of wrong rate on first-anchor acquisition, which then snaps. Low impact, but the invariant "this is a safety clock" argues for at least a MAC or checksum.

<details>
<summary><sub><i>Primer 5 — MAC, HMAC, DPAPI</i></sub></summary>

<sub><b>MAC</b> (<i>Message Authentication Code</i>). A short tag computed from the message plus a secret key; the tag detects tampering by anyone who doesn't hold the key.</sub>

<sub><b>HMAC-SHA256</b>. A specific, well-analysed MAC construction using the SHA-256 hash function.</sub>

<sub><b>DPAPI</b> (Windows Data Protection API, <code>CryptProtectData</code>). Encrypts a blob with a key derived from the user's logon credentials, stored by the OS. Simpler than managing our own keys; appropriate for "protect this file from other local users and offline attackers".</sub>

</details>

**S6. No NTS KE replay / freshness discipline beyond TLS 1.3.**
We rely fully on the TLS 1.3 client-hello entropy + NTS cookie for replay protection. Correct by the spec, but there is no independent *freshness* bound: if an attacker can force a TLS session resumption with a stale ticket (and the provider mis-implements ticket rotation), the resulting timestamp could predate the attack. Low likelihood — but verifiable by having clock.c reject any `ntpUtcMs` that decreases by > X seconds relative to the previous OK cycle.

<details>
<summary><sub><i>Primer 6 — TLS 1.3 replay, session resumption, 0-RTT</i></sub></summary>

<sub><b>Replay attack</b>. Attacker captures a valid encrypted message and replays it later. TLS 1.3 defends against replay of <i>handshakes</i> with fresh randoms and against replay of <i>records</i> with AEAD nonces.</sub>

<sub><b>Session resumption / 0-RTT</b>. To make reconnects fast, TLS 1.3 lets a client attach application data to the first flight using a pre-shared key from a prior session. 0-RTT data is <i>not</i> guaranteed non-replayable by the spec — the application must cope. NTS addresses this with its own cookie discipline; mis-implementations on the server side have been observed.</sub>

</details>

**S7. No bound on single-cycle rate *target*.**
`corr` is clamped to ±2000 ppm before the EMA, so a single malicious-but-gate-passing cycle can pull the EMA by up to (2000 − current) × 0.25. Over 2–3 carefully-shaped cycles an adversary who can defeat the gate once could drive the persisted ppm to ±500 (the hard clamp). This is the "pulse a cycle, wait, pulse again" variant. Defense: require *N consecutive* OK cycles before a rate update is committed to disk, plus a per-cycle **change** clamp (e.g. |Δppm| ≤ 20 per cycle).

---

## 3. Correctness analysis

<details>
<summary><sub><i>Primer 7 — monotonicity, projection, RTT asymmetry</i></sub></summary>

<sub><b>Monotonicity</b>. A clock is monotonic if it never moves backwards. Displayed analog clocks that jump backwards are disconcerting and, in a safety context, dangerous (events get reordered).</sub>

<sub><b>Projection</b>. Given two samples at different QPC moments, "project" one onto the other means "advance / retard it by the elapsed QPC ticks times the current rate" so the two are comparable.</sub>

<sub><b>RTT asymmetry</b>. NTP assumes the forward leg and return leg of the round trip take equal time. On transoceanic or multi-ISP paths this is often false; the error shows up as a constant bias in <code>ntpUtcMs</code> equal to half the asymmetry.</sub>

</details>

**C1. First-anchor trust escape.** On the very first OK cycle we accept `(ntpUtcMs, qpcAtT4)` verbatim — no consistency cross-check is possible because there is no prior anchor. Combined with a stale persisted rate (up to +57 ppm observed in the live log), the first ~10 seconds of displayed time can be off by "rate × interval since anchor" before the cycle-2 slew corrects it. For a safety-critical clock this is a visible defect: the moment the user opens the app is the moment the clock is least trustworthy.

**C2. RTT asymmetry assumption.** `ntpUtcMs = t3 + netRtt/2`. Asymmetric routing makes the error up to netRtt/2 — for NICT (Tokyo, rtt ≈ 300 ms) that is ±150 ms worst case. The 200 ms gate barely covers this; we observe 14 ms spread in happy-path, but on a bad day NICT alone could push us over the gate, forcing INOP.

**C3. EMA on ppm, but rate samples are not IID.** A short interval plus a large residual produces a huge `corr`. The `elapMs > 30000` guard prevents the trivial blow-up but does nothing about the much more common "85 s cycle, residual 9 ms" → corr ≈ 106 ppm, pulled by α = 0.25 → +26 ppm jump. The live log shows ±8–34 ppm jumps between consecutive cycles — the PLL **chasing network jitter**, not drift. Proper approach: a **PI controller** or small Kalman filter with an explicit measurement-noise variance. Chronyd and NTPsec have 20 years of tuning we could borrow.

<details>
<summary><sub><i>Primer 8 — IID, jitter, PI controller, Kalman filter</i></sub></summary>

<sub><b>IID</b> (Independent and Identically Distributed). Statistics assumption: each sample is drawn from the same distribution, independently of the others. A simple EMA implicitly assumes this. NTP samples are <i>not</i> IID — they share route state, server load, and so on — which makes a naive EMA noisy.</sub>

<sub><b>Jitter</b>. Short-term variability in round-trip time or offset. Distinct from <i>drift</i>, which is long-term, monotonic deviation of the local oscillator's rate.</sub>

<sub><b>PI controller</b> (Proportional-Integral). Classical control primitive: <code>u = Kp·error + Ki·∫error dt</code>. Proportional term rejects immediate error; integral term removes steady-state bias. Standard choice for clock-discipline loops; tune Kp/Ki for desired bandwidth and damping.</sub>

<sub><b>Kalman filter</b>. Optimal (under Gaussian noise) state estimator that tracks the joint distribution of phase, frequency, and optionally drift, updating with each new measurement and its covariance. Strictly more powerful than a PI controller but more parameters to get right.</sub>

<sub><b>chronyd, NTPsec</b>. Two well-maintained open-source reference implementations of NTP clients; their disciplining loops are documented and battle-tested.</sub>

</details>

**C4. Slew is linear; clocks run smooth but non-monotonic.** If a new slew overrides an in-progress slew, the displayed time can briefly go *backward*. Slew-over-slew should either (a) complete the old slew into the anchor first, then start the new one, or (b) superimpose slews. Today neither happens — the old slew is discarded mid-flight, leaving a small residual that the next sync rediscovers.

**C5. No sanity bound on anchor jump.** An NTS provider returning 2020 would be accepted without cross-check against the previous anchor. The 200 ms concurrence gate requires *core* agreement, so in practice this is caught — but if 2-of-3 core sources also return 2020 (hypothetical coordinated attack or cached resolver result) we snap to 2020 silently. A monotonic bound "new anchor cannot predate the last OK anchor by more than (elapsed_qpc + 60 s slack)" would catch this class deterministically.

**C6. Rate clamp is rectangular.** Real crystal oscillators drift on the order of ±50 ppm; ±500 ppm is 10× observed. A rate target beyond ±100 ppm is almost certainly an error (bad sync pair, sleep-wake with stale QPC, malicious sample). The clamp should be narrower by default and widen only on confirmed sustained drift.

**C7. No test coverage of the adjustment math under adversarial inputs.** Tests verify the concurrence gate and projection arithmetic, but no test feeds `Clock_OnSyncedNtpUtc` a time series with injected jitter / bias / jumps and asserts stability. For a safety clock this is the single biggest gap.

---

## 4. Availability analysis

<details>
<summary><sub><i>Primer 9 — availability, DoS, back-off, dual-stack</i></sub></summary>

<sub><b>Availability</b>. The fraction of time a service performs its intended function. For Lunar: the fraction of clock-wall time during which the UI can render a <i>trusted</i> time (green) rather than INOP (red).</sub>

<sub><b>DoS</b> (Denial of Service). Any attack or failure mode that reduces availability without necessarily compromising integrity. Lunar's "INOP when uncertain" stance converts many <i>integrity</i> threats into <i>availability</i> problems, which is correct but makes DoS the dominant failure mode.</sub>

<sub><b>Back-off</b>. Lengthening the retry interval when failures persist, to avoid hammering a degraded service. Canonical pattern: exponential back-off with jitter.</sub>

<sub><b>Dual-stack</b>. Supporting both IPv4 and IPv6 transparently, so a host reachable only over one of them still works.</sub>

</details>

**A1. Hard dependency on NTS availability.** See S1. Real-world NTS providers (Cloudflare, Netnod, SIDN, PTB, NetTime) have had multi-hour outages; a coincident outage of two providers within our small pool puts us permanently INOP despite three healthy NIST/PTB/NICT sources.

**A2. Aggregator wait is `WaitForMultipleObjects(..., TRUE, 20000)`.** If ANY worker thread is stuck (e.g. a socket call not honoring its timeout due to a Winsock driver bug) the entire cycle blocks for 20 s before the others are harvested. Core workers already completed in ~300 ms sit idle. Fix: wait with `WAIT_ALL=FALSE` and poll, or wait per-source with individual deadlines.

**A3. 5 s cadence during INOP can DoS the providers.** If NIST and NICT are both degraded, we hammer them every 5 s forever. Friendly to us (fast recovery) but hostile to public infrastructure and may earn rate-limiting, prolonging INOP. Recommendation: exponential back-off on INOP (5 → 10 → 20 → 60 s cap), reset to 5 s on first OK.

**A4. No caching of "last known good anchor" for display-only fallback.** Architecturally deliberate (we promise INOP when untrusted), but a **user-facing** availability concession would be: during INOP, render the last-known-good time in amber with "LAST KNOWN: 12 s ago" — still safer than the system clock, still visually distinct from trusted green, and keeps the clock usable during brief network hiccups.

**A5. Cold-start latency.** First OK cycle observed at T+6.3 s. That is the user's *entire* first-impression window rendering INOP. Pre-flighting a fast SNTP-only query against a cached IP (previous cycle's resolved address) while the NTS handshake catches up would shorten this considerably.

**A6. No IPv6.** `hints.ai_family = AF_INET`. On IPv6-only networks (increasingly common in mobile and enterprise) the core sources fail hard.

---

## 5. Proposal (prioritized)

### Tier 1 — security & correctness (ship first)

1. **Tiered trust with graceful degradation** (addresses S1, A1).
   Three tiers:
   - `TRUST_OK` — NTS + ≥ 2 core concur within 200 ms (today's behavior).
   - `TRUST_DEGRADED` — NTS unavailable, but ≥ 3 core sources concur within 100 ms (tighter) AND last NTS-OK is < 2 hours old. Clock continues to run disciplined; UI shows amber "UNAUTHENTICATED" badge; rate is **read-only** (no EMA update from degraded cycles).
   - `TRUST_INOP` — nothing else.

   Availability rises; security stance is explicit (degradation is user-visible, not silent); PLL is protected from being poisoned by unauthenticated samples.

2. **Anchor monotonicity guard** (C5, S6). Reject any new anchor where `ntpUtcMs < previousAnchorUtcMs + elapsedQpcMs − slack` (slack ≈ 60 s). Hard reject: INOP, log `clock: anchor rejected — retrograde time (Δ=…)`.

3. **Per-cycle rate-change clamp** (S7, C3). Limit `|newRate − oldRate| ≤ 20 ppm` per cycle regardless of EMA math. A sustained drift still converges in ~ 5 cycles; a single adversarial pulse cannot swing the rate by > 20 ppm.

4. **Disk integrity on `discipline.dat`** (S5). Wrap the file in DPAPI (`CryptProtectData`). Reject tampered files, fall back to 0-ppm bootstrap, log.

5. **Slew superposition** (C4). When a new residual arrives during an active slew, fold the remaining residual of the old slew into the anchor first, then start the new slew. Guarantees the displayed clock is monotonic within the slew window.

6. **Adversarial-input test suite** (C7). Table-driven tests that feed `Clock_OnSyncedNtpUtc` crafted sequences: steady drift, step change, sleep-wake (QPC gap), alternating-bias attack, slow bias-drift attack. Assert: monotonicity, convergence, bounded rate excursion, no snap-storm.

### Tier 2 — availability

7. **Exponential INOP back-off** (A3): 5 / 10 / 20 / 40 / 60 s, reset on OK.
8. **Parallel-harvest aggregator** (A2): decouple per-source deadlines so core replies are processed the instant they arrive; NTS slot runs on its own 20 s budget.
9. **IPv6 dual-stack** (A6): `AF_UNSPEC` + try-all.
10. **Last-known-good amber mode** (A4): render recent-but-stale time distinctly, not INOP, for outages ≤ 2 min. Guarded by the TRUST_DEGRADED tier above.
11. **Warm-start pre-flight SNTP** (A5): one unauthenticated query against the cached IP of the fastest previous source, fired in parallel with `Init`. Displayed as "SYNCING" (untrusted) until the first gated OK lands.

### Tier 3 — defense-in-depth

12. **Confidence-weighted concurrence.** Gate passes at 200 ms, but log a `confidence = 1 − (spread/200)` score; display HIGH / MED / LOW in About. Suspicious-but-passing cycles (< 50 % confidence for N consecutive cycles) raise a user-visible warning without forcing INOP.
13. **NTS pool health tracking.** Per-provider EWMA of RTT, handshake success rate, outlier rate. Provider weight ∝ health; a recently-faulting provider gets 0 weight for N cycles.
14. **Replace ppm-EMA with PI controller (or small Kalman)** (C3). Well-understood literature; chronyd is the reference. Lower steady-state wander, faster convergence, rejects measurement noise better.
15. **Kiss-o'-Death handling.** We reject stratum 0 but do not parse the reference-ID kiss code. Respect `RATE` / `DENY`; apply a provider-specific cooldown.
16. **Add a 5th slot: Roughtime** (future). Independent protocol (Cloudflare / Google). Converts NTS from single cryptographic anchor to 2-of-2 cryptographic anchors.

<details>
<summary><sub><i>Primer 10 — Kiss-o'-Death, Roughtime, EWMA</i></sub></summary>

<sub><b>Kiss-o'-Death (KoD)</b>. An NTP server reply with stratum 0 and a 4-letter reference-ID code (e.g. <code>RATE</code>, <code>DENY</code>) asking the client to stop or slow down. Respecting these keeps us in good standing with public servers.</sub>

<sub><b>Roughtime</b>. An independently-designed secure time protocol using Ed25519 signatures with explicit "I will misbehave — here is the proof" auditability. Complements NTS by being based on an entirely different cryptographic stack and operator ecosystem.</sub>

<sub><b>EWMA</b> (Exponentially Weighted Moving Average). Same as EMA; the term "EWMA" is common in quality-control and health-monitoring contexts. Used here for per-provider health scores.</sub>

</details>

---

## 6. Recommendation

Start with **Tier 1 items 1–3 + 6** (tiered trust, anchor monotonicity, rate-change clamp, adversarial test suite). They directly address the highest-severity findings (S1, S7, C3, C5, C7), are each ~150–400 LOC, and the test suite is the investment that makes all subsequent changes safe.

Tier 2 availability work should be gated behind Tier 1: "improving availability" without tighter safety gates would amplify any latent correctness bug.

Tier 3 is long-term hygiene — take one item per iteration once Tier 1 and Tier 2 have landed.

---

<details>
<summary><sub><i>Appendix — glossary of all abbreviations used</i></sub></summary>

<sub><b>AEAD</b> — Authenticated Encryption with Associated Data.</sub>
<sub><b>API</b> — Application Programming Interface.</sub>
<sub><b>CA</b> — Certificate Authority.</sub>
<sub><b>DNS / DNSSEC</b> — Domain Name System / DNS Security Extensions.</sub>
<sub><b>DoS</b> — Denial of Service.</sub>
<sub><b>DPAPI</b> — Data Protection API (Windows).</sub>
<sub><b>EMA / EWMA</b> — Exponential (Weighted) Moving Average.</sub>
<sub><b>HMAC</b> — Hash-based Message Authentication Code.</sub>
<sub><b>IID</b> — Independent and Identically Distributed.</sub>
<sub><b>INOP</b> — Inoperative.</sub>
<sub><b>KE</b> — Key Establishment (the TLS-1.3 handshake phase of NTS).</sub>
<sub><b>KoD</b> — Kiss-o'-Death (NTP control reply).</sub>
<sub><b>LI / VN / Mode</b> — Leap Indicator / Version Number / Mode (NTP header fields).</sub>
<sub><b>LOC</b> — Lines Of Code.</sub>
<sub><b>MAC</b> — Message Authentication Code.</sub>
<sub><b>NICT</b> — National Institute of Information and Communications Technology (Japan).</sub>
<sub><b>NIST</b> — National Institute of Standards and Technology (USA).</sub>
<sub><b>NTP / SNTP</b> — Network Time Protocol / Simple NTP.</sub>
<sub><b>NTS</b> — Network Time Security (RFC 8915).</sub>
<sub><b>PI controller</b> — Proportional-Integral feedback controller.</sub>
<sub><b>PLL</b> — Phase-Locked Loop.</sub>
<sub><b>ppm</b> — parts per million.</sub>
<sub><b>PTB</b> — Physikalisch-Technische Bundesanstalt (Germany).</sub>
<sub><b>QPC</b> — QueryPerformanceCounter (Windows monotonic timer).</sub>
<sub><b>RFC</b> — Request For Comments (IETF standards document).</sub>
<sub><b>RST</b> — TCP Reset packet.</sub>
<sub><b>RTT</b> — Round-Trip Time.</sub>
<sub><b>SID</b> — Security Identifier (Windows).</sub>
<sub><b>SPKI</b> — Subject Public Key Info (a cryptographic pinning target).</sub>
<sub><b>TLS</b> — Transport Layer Security.</sub>
<sub><b>UDP</b> — User Datagram Protocol.</sub>
<sub><b>UTC</b> — Coordinated Universal Time.</sub>

</details>
