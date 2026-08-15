import argparse
from pathlib import Path


POLICIES = (
    "fixed-minimum",
    "fixed-class-aware",
    "biological-adaptive",
)
CONDITIONS = ("timed-replay-v2", "regime-change-v1")
DEADLINE_SEMANTICS = "descriptor-relative-receiver-steady"


def parse_record(line):
    fields = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def records(text, prefix):
    return [
        parse_record(line)
        for line in text.splitlines()
        if line.startswith(prefix + " ")
    ]


def integer(record, key):
    try:
        return int(record[key])
    except (KeyError, ValueError) as error:
        raise RuntimeError(f"invalid or missing {key}: {record}") from error


def real(record, key):
    try:
        return float(record[key])
    except (KeyError, ValueError) as error:
        raise RuntimeError(f"invalid or missing {key}: {record}") from error


def indexed(records_value, role):
    result = {integer(record, "index"): record for record in records_value}
    if set(result) != set(range(8)) or len(records_value) != 8:
        raise RuntimeError(f"{role} evidence is not the fixed eight generations")
    return result


def protection(record):
    return tuple(
        real(record, key)
        for key in (
            "critical_protection_factor",
            "important_protection_factor",
            "elastic_protection_factor",
        )
    )


def verify_receiver_deadlines(receiver_text, condition):
    ready = records(receiver_text, "receiver_ready")
    if len(ready) != 1:
        raise RuntimeError("missing unique receiver readiness evidence")
    expected_outage = "2" if condition == "regime-change-v1" else "none"
    if ready[0].get("deadline_semantics") != DEADLINE_SEMANTICS:
        raise RuntimeError("receiver readiness lacks local deadline semantics")
    if ready[0].get("regime_outage_generation_index") != expected_outage:
        raise RuntimeError("receiver readiness has the wrong regime schedule")

    by_index = indexed(
        records(receiver_text, "receiver_generation_complete"), "receiver"
    )
    for index, record in by_index.items():
        if record.get("deadline_clock") != "receiver-steady-descriptor-relative":
            raise RuntimeError(f"generation {index} used the wrong deadline clock")
        received = integer(record, "descriptor_received_at_ms")
        critical_duration = integer(record, "critical_deadline_duration_ms")
        critical_at = integer(record, "critical_deadline_at_ms")
        generation_duration = integer(record, "generation_deadline_duration_ms")
        generation_at = integer(record, "generation_deadline_at_ms")
        if critical_duration != 10_000 or generation_duration != 30_000:
            raise RuntimeError("descriptor-relative deadline duration changed")
        if critical_at != received + critical_duration:
            raise RuntimeError("critical deadline was not receiver-anchored")
        if generation_at != received + generation_duration:
            raise RuntimeError("generation deadline was not receiver-anchored")
    return by_index


def verify_case(sender_text, receiver_text, policy, condition):
    if policy not in POLICIES or condition not in CONDITIONS:
        raise RuntimeError("unknown policy pilot evidence identity")
    sender = indexed(
        records(sender_text, "sender_generation_complete"), "sender"
    )
    receiver = verify_receiver_deadlines(receiver_text, condition)

    for index, record in sender.items():
        if record.get("policy_id") != policy:
            raise RuntimeError(f"generation {index} has the wrong policy")
        protection(record)
        for key in (
            "adaptive_state_present",
            "adaptive_generation_count_at_plan",
            "adaptive_success_count_at_plan",
            "adaptive_failure_count_at_plan",
            "adaptive_panic_boost_at_plan",
            "adaptive_critical_overhead_at_plan",
            "adaptive_important_overhead_at_plan",
            "adaptive_success_count_after_terminal",
            "adaptive_failure_count_after_terminal",
            "adaptive_panic_boost_after_terminal",
        ):
            real(record, key)
        if integer(record, "delivered") != integer(receiver[index], "delivered"):
            raise RuntimeError("sender and receiver delivery evidence disagree")

    if policy.startswith("fixed-"):
        plans = {protection(record) for record in sender.values()}
        if len(plans) != 1:
            raise RuntimeError("fixed policy changed its protection plan")
        if any(integer(record, "adaptive_state_present") != 0
               for record in sender.values()):
            raise RuntimeError("fixed policy emitted adaptive state")
    else:
        if any(integer(record, "adaptive_state_present") != 1
               for record in sender.values()):
            raise RuntimeError("biological policy omitted adaptive state")

    if condition == "regime-change-v1":
        failed = sender[2]
        subsequent = sender[3]
        receiver_failed = receiver[2]
        if (integer(failed, "delivered") != 0 or
                integer(failed, "terminal_failure") != 1 or
                integer(receiver_failed, "regime_outage") != 1 or
                integer(receiver_failed, "regime_suppressed_symbols") <= 0):
            raise RuntimeError("regime change did not cause the declared failure")
        if integer(subsequent, "delivered") != 1:
            raise RuntimeError("regime change did not exercise subsequent planning")
        for index in (0, 1, 3, 4, 5, 6, 7):
            if integer(sender[index], "delivered") != 1:
                raise RuntimeError("regime schedule affected an undeclared generation")
        if policy == "biological-adaptive":
            if protection(subsequent) == protection(failed):
                raise RuntimeError("biological next plan ignored terminal failure")
            if integer(failed, "adaptive_failure_count_after_terminal") != 1:
                raise RuntimeError("biological failure was not applied")
            if integer(subsequent, "adaptive_failure_count_at_plan") != 1:
                raise RuntimeError("next biological plan lacks updated state")
            if integer(subsequent, "adaptive_panic_boost_at_plan") != 2:
                raise RuntimeError("next biological plan lacks causal panic state")
    return {
        "policy": policy,
        "condition": condition,
        "delivered": sum(integer(record, "delivered") for record in sender.values()),
    }


def split_cases(text):
    result = {}
    identity = None
    lines = []
    for line in text.splitlines():
        if line.startswith("remote_case "):
            if identity is not None:
                result[identity] = "\n".join(lines) + "\n"
            fields = parse_record(line)
            identity = (fields.get("condition"), fields.get("policy"))
            lines = []
        elif identity is not None:
            lines.append(line)
    if identity is not None:
        result[identity] = "\n".join(lines) + "\n"
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sender", type=Path, required=True)
    parser.add_argument("--receiver", type=Path, required=True)
    parser.add_argument("--policy", choices=POLICIES)
    parser.add_argument("--condition", choices=CONDITIONS)
    parser.add_argument("--combined", action="store_true")
    args = parser.parse_args()
    sender_text = args.sender.read_text(encoding="utf-8")
    receiver_text = args.receiver.read_text(encoding="utf-8")
    if args.combined:
        sender_cases = split_cases(sender_text)
        receiver_cases = split_cases(receiver_text)
        expected = {(condition, policy)
                    for condition in CONDITIONS for policy in POLICIES}
        if set(sender_cases) != expected or set(receiver_cases) != expected:
            raise RuntimeError("combined evidence is not the six pilot cells")
        for condition, policy in sorted(expected):
            verify_case(
                sender_cases[(condition, policy)],
                receiver_cases[(condition, policy)],
                policy,
                condition,
            )
    else:
        if args.policy is None or args.condition is None:
            parser.error("--policy and --condition are required without --combined")
        verify_case(sender_text, receiver_text, args.policy, args.condition)
    print("policy pilot deadline and causal adaptation evidence passed")


if __name__ == "__main__":
    main()
