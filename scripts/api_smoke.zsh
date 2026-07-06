#!/bin/zsh
# Smoke-tests the Rewair web API against a live device.
# Usage: REWAIR_IP=192.168.x.x scripts/api_smoke.zsh
set -e
IP="${REWAIR_IP:?set REWAIR_IP to the device address}"
BASE="http://$IP"
fail=0

check() {
    local desc="$1"; shift
    if "$@" > /dev/null 2>&1; then
        echo "ok   $desc"
    else
        echo "FAIL $desc"
        fail=1
    fi
}

status_json="$(curl -sf "$BASE/api/status")"
check "status has score.value"      jq -e '.score.value | numbers' <<< "$status_json"
check "status has sens.temp"        jq -e '.sens.temp | numbers' <<< "$status_json"
check "status has sens.light"       jq -e '.sens.light | numbers' <<< "$status_json"
check "status wifi mode sta or ap"  jq -e '.wifi.mode == "sta" or .wifi.mode == "ap"' <<< "$status_json"
check "status has time.valid"       jq -e '.time | has("valid")' <<< "$status_json"
check "status has settings.tz_posix" jq -e '.settings.tz_posix | strings' <<< "$status_json"

check "scan returns array"          sh -c "curl -sf '$BASE/api/scan' | jq -e 'type == \"array\"'"
check "networks returns array"      sh -c "curl -sf '$BASE/api/networks' | jq -e 'type == \"array\"'"

check "update returns 501"          sh -c "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{}' '$BASE/api/update')\" = 501 ]"
check "disp rejects bad mode (400)" sh -c "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{\"mode\":\"bogus\"}' '$BASE/api/disp')\" = 400 ]"
check "time rejects tiny epoch"     sh -c "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{\"epoch\":5}' '$BASE/api/time')\" = 400 ]"
check "disp accepts text/plain (204)" sh -c "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: text/plain' -d '{\"mode\":\"score\"}' '$BASE/api/disp')\" = 204 ]"
check "status rejects POST (405)"   sh -c "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$BASE/api/status')\" = 405 ]"

check "sse first event < 3s"        sh -c "curl -sN --max-time 3 '$BASE/api/events' | head -1 | grep -q '^data: {'"

exit $fail
