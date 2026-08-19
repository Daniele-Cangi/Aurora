# Contributing to Aurora

Aurora welcomes contributions that make the transport system easier to **test, falsify, compare, replay, or operate without weakening its evidence boundaries**.

Before starting substantial work, read:

1. [`README.md`](README.md)
2. [`AURORA_MASTER_PLAN.md`](AURORA_MASTER_PLAN.md)
3. [`docs/OPEN_RESEARCH_FRONTIER.md`](docs/OPEN_RESEARCH_FRONTIER.md)

Aurora is an active systems-research project. External contributions are not expected to follow the current implementation literally. Alternative controllers, codecs, verification approaches, orchestration designs, and negative results are welcome when their semantics are explicit and the comparison is fair.

## Project invariants

These are stronger than any individual implementation choice.

### One generation, one source of truth

Encoding, transport, decoding, health, and replay must interpret the same immutable generation identity and descriptor. Do not introduce a parallel success path that can disagree with the authoritative decoder.

### Delivery is factual

A delivery claim must come from reconstructed required bytes plus the declared integrity check. Health/progress estimates must not silently become delivery evidence.

### Adaptation is bounded control

A policy may change only declared action variables inside the safety envelope. Record the evidence, previous state, proposed action, constrained action, and outcome needed to replay that decision.

### Safety dominates optimization

No controller, scheduler, learned policy, or exploratory contribution may bypass contract bounds, reserve floors, duty limits, freshness requirements, critical-protection floors, allowed-link constraints, or integrity requirements.

### Evidence levels do not collapse

Keep these distinct:

1. deterministic/unit evidence;
2. stochastic simulation;
3. process/network emulation;
4. hardware-in-the-loop;
5. physical field evidence.

A successful lower-level experiment does not become a calibrated, hardware, regional, or field claim.

### Frozen studies remain frozen

Completed, stopped, invalid, or preregistered study identities are historical evidence. Do not:

- reuse excluded lifecycles;
- replace failed runs;
- mutate a frozen source and retain the old study identity;
- reinterpret a technical failure as a policy outcome;
- retroactively change measurement semantics.

New evidence gets a new experiment/study identity.

## High-value contribution lanes

### 1. Alternative transport controllers

Aurora currently includes fixed policies and the biological-adaptive policy. Useful contributions include bounded, replayable alternatives such as:

- threshold controllers;
- PID-style controllers;
- risk-sensitive controllers;
- model-predictive control;
- contextual-bandit or other online policies;
- deliberately simple baselines that challenge the need for a more complex policy.

A new controller should consume the same declared evidence, pass through the same `SafetyEnvelope`, produce replayable decisions, and be compared on matched traces/workloads.

A contribution that shows the biological policy is unnecessary or inferior is useful evidence.

### 2. Codec and decoder baselines

Useful work includes:

- robust-soliton experiments for the internal LT-like codec;
- sparse/peeling decode before dense elimination;
- malformed-input/property fuzzing;
- Reed-Solomon or other bounded block-code baselines where appropriate;
- external codec adapters that traverse the same descriptor/report lifecycle;
- decode probability, overhead, complexity, and memory measurements across declared seeds and generation sizes.

Do not describe the internal LT-like codec as a standards-compliant replacement for established FEC systems.

### 3. Independent-host and controller portability

The current raw-host research path has exposed substantial controller complexity around provisioning, SSH/IAP session lifetime, platform-specific key handling, teardown, and evidence retrieval.

High-value contributions include:

- provider-neutral remote-host orchestration;
- explicit controller transport/session abstractions;
- resilient detached-process control;
- local/LAN/Tailscale/CI approaches that preserve independent-host evidence without requiring paid cloud runs;
- deterministic fault injection into the controller itself;
- exact artifact retrieval and teardown/audit verification;
- evidence that a simpler independent-host method provides equivalent or better research separation.

**Paid cloud execution is not required for contribution acceptance.** Prefer plan-only, synthetic, local, container, and CI validation where possible. Do not create billable infrastructure from a pull request or workflow unless the repository owner has explicitly authorized that specific run.

### 4. Benchmark and evidence tooling

Useful contributions include:

- imported impairment traces with clear provenance;
- replay validators;
- canonical artifact manifests;
- measurement-schema validation;
- model-free study integrity checks;
- confidence-interval and uncertainty tooling;
- failure-preserving result aggregation;
- experiment diffing that separates source, treatment, controller, and evidence changes.

The goal is not more ceremony. The goal is to make it difficult to accidentally compare unlike experiments or overstate evidence.

### 5. Scheduling and multi-flow semantics

Open work includes:

- weighted-share scheduling;
- fairness defined on effective service rather than turns alone;
- starvation and deadline interactions;
- concurrent arrivals not aligned to the current periodic quantum;
- mobile or changing-contact nodes;
- replayable multi-flow recovery semantics.

A contribution should define what fairness or service guarantee it is testing before optimizing for it.

## Experimental freedom

You do **not** need to implement the approach described in the master plan if you can make a stronger case for another approach.

When proposing something substantially different, state:

- which problem you are solving;
- which Aurora invariant must remain true;
- what existing behavior/baseline you are comparing against;
- what evidence would count against your approach;
- what becomes simpler, cheaper, more reproducible, or more informative if it works.

Unexpected approaches are welcome. A contribution may replace an existing experimental direction if the comparison is explicit and the evidence supports it.

## Research and PR expectations

For a substantial research change, open an issue first and include:

- research/engineering question;
- current limitation;
- proposed method or contract;
- fixed vs changed factors;
- workload/trace/data provenance;
- seeds/randomization where applicable;
- primary metrics;
- uncertainty treatment;
- claim boundary;
- failure/null outcome;
- whether historical evidence remains comparable.

A pull request should state:

- what changed;
- why;
- which invariant(s) it affects;
- tests/evidence added;
- known limitations;
- whether any measurement or protocol semantics changed;
- whether a new experiment identity is required.

Keep PRs conceptually focused where practical.

## Validation

Use the narrowest relevant validation first, then broader regression gates when needed.

Typical checks include:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Python tooling/tests should be run directly for the subsystem being changed. For process-emulation or controller work, prefer deterministic plan-only and CI gates before any live infrastructure.

Do not treat a green CI run as field validation.

## Good first contributions

Good bounded contributions may include:

- fuzz/property tests for descriptor, process-frame, or trace parsing;
- replay tests for irregular/out-of-order controller events;
- a small imported impairment-trace adapter;
- canonical evidence-manifest validation;
- a simple fixed/threshold controller under the existing replay contract;
- tests for unavailable crypto/auth backends;
- documentation corrections that reduce claim strength to implemented evidence.

## License of contributions

Aurora is licensed under the Apache License, Version 2.0. Unless you explicitly state otherwise, every contribution intentionally submitted for inclusion in Aurora is provided under the same license, as described by Section 5 of the Apache License.

Do not submit code, data, documentation, media, or other material unless you have the right to contribute it under those terms. Identify third-party material explicitly and preserve all applicable notices.

## Developer Certificate of Origin

Each commit must include a `Signed-off-by` trailer certifying the Developer Certificate of Origin 1.1: <https://developercertificate.org/>.

Use Git's sign-off option:

```bash
git commit --signoff
```

The sign-off certifies that you wrote the contribution or otherwise have the right to submit it under the project's license. It is not a copyright assignment.
