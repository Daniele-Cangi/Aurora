# Raw-host policy pilot v1

## Completion record

The frozen design below completed successfully as
`raw-host-policy-pilot-v1-study-v4`: 12/12 valid fresh VM-pair lifecycles, two
complete randomized blocks, no replacements and final independent cleanup
audit zero. The evidence and post-freeze descriptive analysis are published in
the [`raw-host-policy-pilot-v1-study-v4` Release](https://github.com/Daniele-Cangi/Aurora/releases/tag/raw-host-policy-pilot-v1-study-v4),
and the result is documented in
[`raw-host-policy-pilot-v1-final-results`](raw-host-policy-pilot-v1-final-results.md).

This document remains the frozen design record. No workload, trace, policy,
parameter, randomization or measurement rule below has been rewritten after
observing the pilot.

## Status and boundary

This is a bounded policy-discrimination pilot, not a confirmatory study. Its
purpose is to determine whether the authenticated process/raw-host path
produces sufficiently distinct deadline, delivery, redundancy, repair and
feedback observations to justify a separately frozen follow-up. It does not
test policy superiority and it does not designate `biological-adaptive` as the
expected winner.

No GCP execution is part of this change. The campaign runner defaults to a
read-only plan and refuses execution unless the operator supplies both the
billing/teardown acknowledgement and a reviewed source commit identical to the
requested source commit.

The design below was frozen only after the process-separated, Docker and
independent remote-host gates passed. Those gates are validation evidence, not
pilot observations, and did not create GCP resources.

## Implementation audit

The three controllers already existed in `TransportPolicy.hpp`:

- `fixed-minimum` proposes 1.0 for critical, important and elastic segments,
  subject to the transport contract's critical floor and global cap.
- `fixed-class-aware` proposes 2.5, 1.5 and 1.1 respectively, with the same
  contract enforcement.
- `biological-adaptive` uses the existing version-1 biological state machine:
  `alpha_up=0.10`, `alpha_down=0.02`, a three-generation panic budget and the
  existing flow-class bases and maxima.

Previously, `aurora_process_emulation` always constructed the adaptive policy,
planned both smoke generations before receiving terminal feedback, and applied
policy observations only after the whole run. This prevented runtime treatment
selection and prevented feedback from affecting a later plan.

The process binary now uses the same canonical policy factory as the transport
benchmark. The sender accepts `--policy` and `--workload`; neither option
changes compilation or linkage. The pilot runner requires a common
`runtime_binary_sha256` across all twelve lifecycles, while each sender and
receiver pair also verifies the same binary hash.

Pilot generations are planned sequentially after authenticated terminal
feedback for the preceding generation. A terminal outcome may be delivery or
an enforced deadline failure. Exhausting the repair budget no longer turns a
deadline observation into a harness error: bounded authenticated probes remain
active until terminal delivery or expiry. Feedback is applied exactly once
before the next generation is planned.

All receiver deadline outcomes are now evaluated on one receiver
`steady_clock`. The first accepted authenticated descriptor establishes the
local origin; the authenticated descriptor supplies only relative segment and
generation durations. Sender expiry timestamps are never used to decide a
receiver outcome, and the descriptor itself remains immutable.

The completed pre-change remote-host evidence saturated the intended outcome:
all 48 policy-condition generations were delivered, all critical segments met
their deadlines, and the biological state never observed a failure. That
evidence could not discriminate causal adaptation, so the second condition was
replaced before freeze with the policy-neutral regime change described below.

## Frozen design

The authoritative freeze is
[`benchmarks/gcp_raw_host_policy_pilot_v1.json`](../benchmarks/gcp_raw_host_policy_pilot_v1.json).
It declares:

- one topology: `us-east1-b` sender to `us-west1-b` receiver, non-peered VPCs,
  public IPv4, `e2-micro`;
- three runtime policy treatments;
- two adverse conditions assembled from existing versioned transport traces:
  `timed-replay-v2` and `regime-change-v1`;
- two randomized complete blocks, each containing all six policy-condition
  cells;
- exactly 12 fresh VM-pair lifecycles, with no replacement lifecycle;
- the fixed SHA-256 complete-block seed and complete execution order;
- trace paths and SHA-256 digests, exact policy parameters, workload contract,
  and measurement-schema digest.

The fixed `policy-pilot-v1` workload has eight 2,560-byte generations, 64-byte
symbols and the existing segmented transport contract semantics. Each
generation contains 1,024 critical bytes with a 10-second segment deadline,
1,024 important bytes with a 20-second deadline, and 512 elastic bytes under a
30-second generation deadline. Generation index 2 in `regime-change-v1` is a
declared terminal failure and generation index 3 is planned only after that
feedback. The causal regression requires the biological plan to reflect its
updated state and requires both fixed policies to remain invariant.

The conditions reuse existing traces rather than introducing a new workload:

- `timed-replay-v2` uses `process_timed_v2.trace` forward and
  `process_feedback_v2.trace` reverse;
- `regime-change-v1` reuses those same forward and reverse traces, while the
  receiver deterministically suppresses symbol datagrams for generation index
  2 until its receiver-local critical deadline. Authenticated descriptors and
  feedback remain active. The blackout is independent of treatment and forces
  every policy through failure followed by a subsequent plan; it does not tune
  transport conditions to make one policy win.

## Frozen measurements and analysis

The authoritative schema is
[`benchmarks/raw_host_policy_pilot_measurement_v1.json`](../benchmarks/raw_host_policy_pilot_measurement_v1.json).
Each lifecycle records:

- critical generations delivered before the receiver-clock segment deadline;
- complete payload delivery before generation expiry;
- source symbols, initial symbols and actual symbol datagrams;
- requested and emitted repair symbols;
- descriptor retransmission and replay/reordering evidence;
- all-feedback and terminal-feedback RTT summaries on the sender steady clock.
- the critical, important and elastic protection factors for every generation;
- the biological generation/success/failure counters, panic budget and
  critical/important adaptive overhead at planning and after terminal feedback.

Critical and total-delivery deadlines are computed only as receiver descriptor
receipt plus the authenticated relative duration. The evidence contract
explicitly prohibits receiver decisions based on sender-relative expiry values.
For `regime-change-v1`, evidence must show a terminal failure at generation 2,
a subsequent generation-3 plan, changed biological protection reflecting the
failure state, and unchanged fixed-policy protection.

Feedback RTT includes forward service, receiver work, configured reverse
impairment, reverse service and sender polling. It is not one-way or
network-only latency. Cross-host clock subtraction remains prohibited.

Analysis is descriptive by lifecycle, policy-condition cell and complete
block. There is no superiority test, power claim, outcome-dependent stopping
or post-hoc workload/trace/policy change. Any confirmatory study requires a new
freeze after this pilot is complete.

## Validation and eventual execution

Validate the frozen plan locally; this performs no GCP calls:

```bash
python3 tools/gcp_raw_policy_pilot.py \
  --project PROJECT_ID \
  --source-commit "$(git rev-parse HEAD)"
```

CTest covers all three runtime policies in process-separated mode. CI also
runs all six policy-condition cells from one libsodium-enabled Docker image and
from one immutable artifact copied to two independent GitHub-hosted VMs. These
are validation gates, not GCP pilot observations. Before this freeze, local
CTest passed 39/39, the Docker six-cell gate passed, and the independent-host
sender, receiver and combined evidence verifier passed in GitHub Actions run
`31916124563`; the corresponding Ubuntu, Windows and secure-auth gates passed
in run `31916124554`.

The retained remote validation record shows the intended causal chain without
being treated as a pilot result: biological generation 2 failed with factors
`2.5/1.5/1.25`, then generation 3 planned with failure count 1, panic budget 2
and factors `4.0/2.325/1.25`. The fixed-minimum and fixed-class-aware factors
were unchanged across the same failure boundary. The local replay of the
combined sender/receiver evidence passed the frozen deadline and adaptation
verifier.

Only after review, an operator may execute the frozen order with explicit
acknowledgements:

```bash
python3 tools/gcp_raw_policy_pilot.py \
  --project PROJECT_ID \
  --source-commit REVIEWED_SHA \
  --reviewed-source-commit REVIEWED_SHA \
  --execute \
  --acknowledge-billing-and-teardown
```

Every lifecycle provisions and tears down one exact VM pair before the next
begins. The runner stops on invalid evidence or teardown failure and does not
replace the lifecycle automatically.
