# Raw-host policy pilot v1: final descriptive result

## Status

`raw-host-policy-pilot-v1-study-v4` completed the frozen pilot design with two
randomized complete blocks and 12 fresh VM-pair lifecycles. All 12 lifecycles
were technically valid, the frozen order was preserved and no lifecycle was
replaced.

The immutable evidence freeze and the post-freeze descriptive analysis are
published in the
[`raw-host-policy-pilot-v1-study-v4` GitHub Release](https://github.com/Daniele-Cangi/Aurora/releases/tag/raw-host-policy-pilot-v1-study-v4).

## Frozen provenance

- Experimental tag: `raw-host-policy-pilot-v1-study-v4`
- Experimental source: `c523e8020c7d96587ce37638d82d728a2513f04c`
- External controller: `de2bae2da5dacfbf0c0563b2e587cbf5d84a447b`
  after controller-only PR #43
- Runtime binary SHA-256:
  `34d9b78293d2237a701a48ad992f658376374788348363ff4800b103153e625c`
- Workload: `policy-pilot-v1`, eight generations
- Conditions: `timed-replay-v2` and `regime-change-v1`
- Policies: `fixed-minimum`, `fixed-class-aware` and
  `biological-adaptive`
- Authenticated process protocol: V3 with HMAC-SHA-256 through libsodium and
  authenticated terminal ACK

The lightweight experimental tag points directly to the source commit. The
controller merge did not change any policy implementation or parameter,
workload, trace, regime-change perturbation, randomization order or measurement
schema.

## Technical validity and cleanup

- 12/12 lifecycle manifests passed; exclusions and replacements were zero.
- Every receiver process exited with status zero and every detached-control
  probe-failure count was zero.
- Sender and receiver log hashes matched their manifests; all captured stderr
  files were empty.
- Every lifecycle used the same binary hash on sender and receiver.
- Every exact-name teardown succeeded on its first bounded pass with zero
  nonzero delete commands.
- Integrated and independent final audits found zero Aurora instances, disks,
  addresses, snapshots, firewall rules, subnets or networks.
- All 96 generation records evaluated descriptor-relative deadline durations
  entirely on the receiver steady clock: 10 seconds for critical delivery and
  30 seconds for terminal generation delivery. Cross-clock subtraction and
  sender-relative expiry decisions were absent.

## Delivery outcome

| Condition | Policy | Block 1 | Block 2 |
|---|---|---:|---:|
| `timed-replay-v2` | `fixed-minimum` | 8/8 | 8/8 |
| `timed-replay-v2` | `fixed-class-aware` | 8/8 | 8/8 |
| `timed-replay-v2` | `biological-adaptive` | 8/8 | 8/8 |
| `regime-change-v1` | `fixed-minimum` | 7/8 | 7/8 |
| `regime-change-v1` | `fixed-class-aware` | 7/8 | 7/8 |
| `regime-change-v1` | `biological-adaptive` | 7/8 | 7/8 |

For every regime-change run, generations 0–1 delivered 2/2 critical payloads
before deadline, generation 2 experienced the imposed terminal failure and
generations 3–7 delivered 5/5. Every policy recovered at generation 3.

Generation 2 is the policy-neutral perturbation, not a policy outcome. The
pilot therefore did not discriminate critical delivery or recovery time.

## Post-shock response

The informative post-shock window is generations 3–7. It contains 200 source
symbols in every run.

| Policy | Critical delivery B1/B2 | Initial symbols B1/B2 | Repair requested B1/B2 | Repair emitted B1/B2 | Wire symbols B1/B2 | Mean wire/source |
|---|---:|---:|---:|---:|---:|---:|
| `fixed-minimum` | 5/5, 5/5 | 240, 240 | 866, 1070 | 560, 560 | 800, 800 | 4.00× |
| `fixed-class-aware` | 5/5, 5/5 | 365, 365 | 583, 570 | 435, 435 | 800, 800 | 4.00× |
| `biological-adaptive` | 5/5, 5/5 | 490, 490 | 302, 398 | 242, 310 | 732, 800 | 3.83× |

Biological used more proactive protection and less subsequent repair. Relative
to fixed-minimum, its two-block mean used 104.17% more initial symbols, 63.84%
fewer repair requests, 50.71% fewer emitted repairs and 4.25% fewer wire
symbols. Relative to fixed-class-aware the corresponding descriptive
differences were +34.25%, −39.29%, −36.55% and −4.25%.

The net post-shock wire count was lower in one block and tied in the other.
That is an efficiency signal, not a delivery advantage or a confirmatory win.

## Whole-run transport summaries

Values are means across the two blocks.

| Condition | Policy | Initial symbols | Repair emitted | Wire symbols | Terminal feedback RTT | Retry rounds |
|---|---|---:|---:|---:|---:|---:|
| `timed-replay-v2` | `fixed-minimum` | 384 | 875 | 1259 | 189.993 ms | 186.0 |
| `timed-replay-v2` | `fixed-class-aware` | 584 | 673 | 1257 | 211.818 ms | 253.0 |
| `timed-replay-v2` | `biological-adaptive` | 592 | 454 | 1046 | 109.623 ms | 75.0 |
| `regime-change-v1` | `fixed-minimum` | 384 | 885 | 1269 | 379.194 ms | 593.0 |
| `regime-change-v1` | `fixed-class-aware` | 584 | 674.5 | 1258.5 | 202.219 ms | 287.5 |
| `regime-change-v1` | `biological-adaptive` | 712 | 446 | 1158 | 171.654 ms | 203.5 |

Terminal feedback RTT is sender-steady application RTT under the declared
impairment contract. It includes application processing and sender polling and
is neither one-way latency nor a network-only RTT.

## Causal adaptation result

The biological protection and controller-state sequence was identical in both
blocks:

| Generation | Context | Critical / important / elastic | State at plan: success / failure / panic |
|---:|---|---|---|
| 0 | pre-shock | 2.5 / 1.5 / 1.25 | 0 / 0 / 0 |
| 1 | pre-shock | 2.5 / 1.5 / 1.25 | 1 / 0 / 0 |
| 2 | imposed failure | 2.5 / 1.5 / 1.25 | 2 / 0 / 0 |
| 3 | first plan after failure | 4.0 / 2.325 / 1.25 | 2 / 1 / 2 |
| 4 | post-shock | 4.0 / 2.325 / 1.25 | 3 / 1 / 1 |
| 5 | post-shock | 4.0 / 2.325 / 1.25 | 4 / 1 / 0 |
| 6 | post-shock | 2.6 / 1.55 / 1.25 | 5 / 1 / 0 |
| 7 | post-shock | 2.58 / 1.53 / 1.25 | 6 / 1 / 0 |

After generation 2 terminated, the biological state became success/failure/
panic `2/1/3`; the immediately subsequent generation-3 plan reflected that
failure. `fixed-minimum` remained at `1.5/1.0/1.0` and
`fixed-class-aware` remained at `2.5/1.5/1.1` for all generations, with no
adaptive state. The causal adaptation regression therefore passed in the
actual raw-host pilot.

## Evidence and checksums

The Release contains the frozen evidence archive, its checksum sidecar, the
Markdown and JSON analyses and their checksum sidecar.

- Evidence archive SHA-256:
  `bacad78c1c678c64b8c9c605186969d54db33904ef8a347d249b12b1a4112c99`
- Analysis JSON SHA-256:
  `c67e82f01840bb51c6a0b0bd37aecc36b795c74fa48662b056e326c9ac274daa`
- Analysis Markdown SHA-256:
  `179c5f67746d274cf31897db1abb1963a42173fc486de2cbfced9db2d20fe74e`

The evidence archive was hashed before outcome analysis began and remained
byte-identical after analysis.

## Decision boundary

The pilot classifies biological adaptation as **promising for efficiency, not
delivery**. It does not justify a powered confirmatory study that retains
critical delivery as the primary outcome under the same saturated post-shock
condition.

A possible follow-up would require a new freeze and review and could
preregister post-shock wire symbols per delivered critical generation, with
repair emission, repair requests and terminal-feedback retry rounds as
supporting outcomes. If delivery superiority remains the scientific target, a
new policy-neutral condition must first avoid post-shock delivery saturation.

This result is descriptive controlled raw-host emulation evidence. It does not
establish policy superiority, calibrated field performance, network-only or
one-way latency, hardware evidence or general Internet performance.
