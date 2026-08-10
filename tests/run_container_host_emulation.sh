#!/usr/bin/env bash
set -euo pipefail

image="${1:-aurora-process-emulation:local}"
evidence_dir="${2:-process-host-evidence}"
run_suffix="${GITHUB_RUN_ID:-$$}"
network="aurora-process-${run_suffix}"
receiver="aurora-receiver-${run_suffix}"
sender="aurora-sender-${run_suffix}"
receiver_ip="172.30.44.2"
sender_ip="172.30.44.3"
session_id="0123456789abcdef"

mkdir -p "${evidence_dir}"

cleanup() {
  docker rm -f "${sender}" "${receiver}" >/dev/null 2>&1 || true
  docker network rm "${network}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker network create --driver bridge --subnet 172.30.44.0/24 \
  "${network}" >/dev/null

docker run --detach --name "${receiver}" \
  --network "${network}" --ip "${receiver_ip}" \
  "${image}" receiver 0.0.0.0 47001 "${sender_ip}" 47002 2 \
  process_feedback_v2.trace process_auth_test.key "${session_id}" \
  >/dev/null

receiver_ready=false
for _ in $(seq 1 50); do
  if docker logs "${receiver}" 2>&1 | grep -q '^receiver_ready '; then
    receiver_ready=true
    break
  fi
  if [ "$(docker inspect -f '{{.State.Running}}' "${receiver}")" != true ]; then
    break
  fi
  sleep 0.1
done
if [ "${receiver_ready}" != true ]; then
  docker logs "${receiver}" >&2
  echo "receiver did not publish readiness" >&2
  exit 1
fi

set +e
timeout 45s docker run --name "${sender}" \
  --network "${network}" --ip "${sender_ip}" \
  "${image}" sender "${receiver_ip}" 47001 0.0.0.0 47002 \
  process_timed_v2.trace process_auth_test.key "${session_id}" \
  >"${evidence_dir}/sender.log" 2>&1
sender_status=$?
set -e

set +e
receiver_status="$(timeout 20s docker wait "${receiver}")"
receiver_wait_status=$?
set -e
if [ "${receiver_wait_status}" -ne 0 ]; then
  receiver_status=124
fi
docker logs "${receiver}" >"${evidence_dir}/receiver.log" 2>&1

cat >"${evidence_dir}/manifest.txt" <<EOF
evidence_version=1
topology=docker-bridge-distinct-network-namespaces
sender_ip=${sender_ip}
receiver_ip=${receiver_ip}
forward_port=47001
reverse_port=47002
session_id=${session_id}
authentication_profile=hmac-sha256-libsodium
receiver_ready=${receiver_ready}
startup_timeout_ms=60000
service_timeout_ms=15000
sender_exit=${sender_status}
receiver_exit=${receiver_status}
EOF

test "${sender_status}" -eq 0
test "${receiver_status}" -eq 0
grep -q '^receiver_ready .*startup_timeout_ms=60000 .*service_timeout_ms=15000 ' \
  "${evidence_dir}/receiver.log"
grep -q "sender_complete generations=2" "${evidence_dir}/sender.log"
grep -q "feedback_applied=2" "${evidence_dir}/sender.log"
grep -q "auth_profile=hmac-sha256-libsodium" \
  "${evidence_dir}/sender.log"
grep -Eq "replay_rejected=[1-9][0-9]*" "${evidence_dir}/sender.log"
grep -q "receiver_complete generations=2" "${evidence_dir}/receiver.log"
grep -q "auth_profile=hmac-sha256-libsodium" \
  "${evidence_dir}/receiver.log"
grep -Eq "replay_rejected=[1-9][0-9]*" "${evidence_dir}/receiver.log"

cat "${evidence_dir}/sender.log"
cat "${evidence_dir}/receiver.log"
