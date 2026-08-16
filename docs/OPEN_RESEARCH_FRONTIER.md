# Aurora-X Open Research Frontier

Aurora-X already contains a large implemented and measured surface. The most useful external contributions are therefore not generic feature additions. They are **independent ways to challenge, simplify, extend, or validate the current architecture**.

This document lists the areas where outside work can add the most information without requiring the contributor to reproduce Aurora's entire development history.

## Current boundary

The repository already has:

- a causal generation/descriptor/decode-report lifecycle;
- bounded adaptive transport policies;
- a hard safety envelope;
- deterministic simulation and replay;
- process-separated authenticated UDP emulation;
- independent-host CI evidence;
- retained raw-host/GCP studies with frozen provenance;
- an internal LT-like codec plus an external Wirehair comparison path;
- evidence and claim boundaries that distinguish simulation, emulation, hardware, and field evidence.

The project should not be expanded merely because another feature is possible.

The questions below are intentionally selected because a contributor may know a better method than the current implementation.

---

## Frontier A — What should control adaptation?

The current biological-adaptive policy is an implemented research controller, not the definition of Aurora.

Open questions:

- Can a bounded PID-style controller produce more stable behavior under the same traces?
- Does a risk-sensitive controller handle deadline/criticality trade-offs more cleanly?
- Is model-predictive control useful enough to justify its complexity?
- Can a very simple threshold controller match the biological policy?
- When does online exploration help, and when does it only add variance?
- What should the controller optimize when delivery, critical delivery, energy, duty, latency, and repair overhead disagree?

A negative result against the current adaptive policy is useful.

**Required comparison principle:** same declared workload, evidence, action bounds, and safety envelope.

---

## Frontier B — Can Aurora preserve independent-host evidence without recurring cloud friction?

Recent raw-host work demonstrated that independent machines introduce important evidence that process/container tests cannot provide, but also substantial orchestration cost and technical failure modes.

The current controller has had to handle:

- platform-specific SSH key formats;
- transient SSH/IAP session failures;
- detached receiver lifetime;
- exact raw-log hashing;
- terminal protocol recovery;
- bounded teardown and residual-resource audit;
- stopped-study provenance when the transport experiment never validly begins.

Open questions:

- Can the host-control layer be provider-neutral?
- Can a contributor reproduce the same independence boundary with GitHub-hosted VMs, Tailscale, LAN hosts, self-hosted runners, or another low-cost setup?
- What is the smallest controller state machine that distinguishes remote-process state from control-session state?
- Can teardown/audit and artifact retrieval be specified independently of GCP?
- Which failures belong to transport evidence and which belong only to controller evidence?
- Can remote-host experiments be replayed or fault-injected without billable infrastructure?

A solution that removes paid-cloud dependence while preserving or strengthening the evidence boundary is particularly valuable.

**Default rule:** no billable infrastructure is required or authorized for a contribution.

---

## Frontier C — How strong is the internal fountain-code path?

Aurora's internal codec is deliberately described as LT-like. It should earn its place through explicit comparison, not naming.

Open questions:

- What changes when the current degree distribution is replaced by a robust soliton distribution?
- How much memory/time can a peeling or sparse decoder save before dense elimination is necessary?
- Where does the internal codec fail across generation size, loss pattern, duplication, and reordering?
- Which malformed descriptors/seeds/degrees produce unsafe or ambiguous behavior?
- How does it compare with Wirehair, Reed-Solomon, repetition, or another relevant baseline under identical application contracts?
- Does differentiated critical/bulk protection improve useful outcomes enough to justify additional complexity?

Useful evidence includes failure curves, not just successful decodes.

---

## Frontier D — Can the evidence system become simpler and stronger?

Aurora intentionally preserves failed and stopped studies. That creates a growing provenance surface.

Open questions:

- Can experiment identity be represented by a small canonical manifest?
- Can source/treatment/controller/evidence changes be diffed mechanically?
- Can a model-free validator detect accidental comparison of incompatible studies?
- Can raw artifact hashes, measurement-schema versions, workload identities, randomization, and result summaries be bound without adding excessive process ceremony?
- Can stopped or invalid studies remain discoverable without cluttering the main research narrative?
- Which evidence should be immutable, and which metadata may be corrected later without changing scientific meaning?

A contribution that removes complexity while making overclaiming harder is preferred over a larger provenance system.

---

## Frontier E — What does fairness mean when service is not equal to scheduling opportunity?

Aurora already records scheduler turns separately from HAL-accepted effective service. The current fairness bound applies to turns, not delivered work.

Open questions:

- Should fairness be defined on turns, attempted service, accepted service, delivered bytes, deadline survival, or another quantity?
- How should NERVE/GLAND/MUSCLE priorities interact with starvation bounds?
- What happens under concurrent arrivals and changing contact windows?
- Can weighted-share policies preserve critical deadlines without making bulk traffic effectively unserviceable?
- How should fairness interact with energy and duty constraints?

The metric definition is part of the research contribution. Do not optimize a fairness score before defining what it means.

---

## Frontier F — Imported reality without premature field claims

The simulator and process-emulation layers are strong enough that new realism should enter as **declared evidence inputs**, not as stronger wording.

Possible contributions:

- imported packet-loss/reordering/delay traces;
- replay of publicly available network measurements where licensing permits;
- reproducible LAN/WAN impairment captures;
- calibration harnesses that compare synthetic and observed traces;
- explicit measurement-coverage and uncertainty metadata.

Imported traces do not automatically make the simulator field-valid. Their origin, transformations, clock semantics, missing data, and applicability must remain explicit.

---

## Choosing a contribution

A good Aurora contribution usually has this shape:

```text
one bounded question
    ↓
one explicit comparison
    ↓
one replayable evidence path
    ↓
a result that may support or contradict the current design
```

Avoid proposals whose main value is adding another architectural layer without a measurement question.

If your approach would substantially change the system, open an issue first. Explain why the current path is insufficient and what evidence would convince you that your alternative is better.

Aurora does not need contributors to agree with its current choices. It needs contributions that make those choices easier to test.
