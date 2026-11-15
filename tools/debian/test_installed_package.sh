#!/usr/bin/env bash
set -euo pipefail

: "${BASE_REVISION:?BASE_REVISION must be set}"
: "${UPGRADE_REVISION:?UPGRADE_REVISION must be set}"

export DEBIAN_FRONTEND=noninteractive

# A disposable Docker container does not run systemd as PID 1. Exercise the
# installed binary explicitly, but block package-manager service starts.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 0755 /usr/sbin/policy-rc.d

apt-get update -o APT::Update::Error-Mode=any
apt-get install --yes --no-install-recommends curl python3 util-linux

shopt -s nullglob
first_packages=(/packages/aowis-server-dvmdb_*"+deb${BASE_REVISION}"_*.deb)
upgrade_packages=(/packages/aowis-server-dvmdb_*"+deb${UPGRADE_REVISION}"_*.deb)
shopt -u nullglob
if (( ${#first_packages[@]} != 1 || ${#upgrade_packages[@]} != 1 )); then
    printf 'Error: expected exactly one base and upgrade package (%d, %d).\n' \
        "${#first_packages[@]}" "${#upgrade_packages[@]}" >&2
    exit 1
fi
first_package="${first_packages[0]}"
upgrade_package="${upgrade_packages[0]}"

printf '\n== Fresh DVMDB package installation ==\n'
apt-get install --yes --no-install-recommends "$first_package"
dpkg-query -W -f='${Status}\n' aowis-server-dvmdb | grep -Fxq 'install ok installed'

binary=/usr/bin/aowis-server-dvmdb
unit=/usr/lib/systemd/system/aowis-server-dvmdb.service
config=/etc/aowis/aowis-server-dvmdb.ini
state=/var/lib/aowis-dvmdb
log_file="$(mktemp /tmp/aowis-dvmdb-package-test.XXXXXXXX)"
port=18755

test -x "$binary"
test -f "$unit"
test -f "$config"
getent passwd aowis-dvmdb >/dev/null
getent group aowis-dvmdb >/dev/null

grep -Fq 'ExecStart=/usr/bin/aowis-server-dvmdb --config /etc/aowis/aowis-server-dvmdb.ini' "$unit"
grep -Fxq 'root=/var/lib/aowis-dvmdb' "$config"
# Production systemd defaults to all interfaces. The Docker qualification
# itself passes --bind 127.0.0.1 to avoid exposing its unauthenticated test
# server when the Docker wrapper runs with --test-network host.
grep -Fxq 'bind=0.0.0.0' "$config"
grep -Fxq 'StateDirectory=aowis-dvmdb' "$unit"
systemd-analyze verify "$unit"

# systemd normally creates StateDirectory= on service start; reproduce it
# here because systemd is not PID 1 in the qualification container.
install -d -o aowis-dvmdb -g aowis-dvmdb -m 0700 "$state"
server_pid=''
cleanup_server()
{
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=''
    fi
}
cleanup()
{
    cleanup_server
    rm -f -- "$log_file"
}
trap cleanup EXIT

start_server()
{
    : > "$log_file"
    setpriv --reuid "$(id -u aowis-dvmdb)" --regid "$(id -g aowis-dvmdb)" --init-groups \
        "$binary" --config "$config" --bind 127.0.0.1 --port "$port" \
        >"$log_file" 2>&1 &
    server_pid=$!
    local ready=false
    local attempt
    for attempt in $(seq 1 60); do
        if curl --silent --fail --max-time 2 \
            "http://127.0.0.1:$port/dvmdb/v1/ready" > /dev/null; then
            ready=true
            break
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if [[ "$ready" != true ]]; then
        cat "$log_file" >&2
        echo 'Error: installed DVMDB server did not reach readiness.' >&2
        exit 1
    fi
}

printf '\n== Installed DVMDB binary HTTP smoke test ==\n'
start_server

project_id="$(python3 - "$port" <<'PY'
import json
import sys
import urllib.request
port = sys.argv[1]
base = f'http://127.0.0.1:{port}'
for path, expected in [
    ('/dvmdb/v1/health', {'status': 'ok'}),
    ('/dvmdb/v1/ready', {'status': 'ready'}),
    ('/dvmdb/v1/projects', {'projects': []}),
]:
    with urllib.request.urlopen(base + path, timeout=5) as reply:
        assert reply.status == 200, (path, reply.status)
        response = json.load(reply)
    assert response == expected, (path, response)

# Store a real unborn project in the installed server, not just a dummy file.
request = urllib.request.Request(
    base + '/dvmdb/v1/projects',
    data=json.dumps({'name': 'Package smoke', 'unborn': True}).encode(),
    headers={'Content-Type': 'application/json'},
    method='POST',
)
with urllib.request.urlopen(request, timeout=5) as reply:
    assert reply.status == 201, reply.status
    created = json.load(reply)
project_id = created['id']
assert created['name'] == 'Package smoke'
with urllib.request.urlopen(base + '/dvmdb/v1/projects', timeout=5) as reply:
    projects = json.load(reply)['projects']
assert len(projects) == 1 and projects[0]['id'] == project_id
print(project_id)
PY
)"
# Verify the unprivileged service could initialize its configured data root.
test -d "$state"
test "$(stat -c '%U:%G' "$state")" = aowis-dvmdb:aowis-dvmdb
cleanup_server

printf '\n== Upgrade preserves configuration and persistent database ==\n'
cat >> "$config" <<'EOF_ADMIN'
; Administrator's local marker (must survive package upgrades and removal).
EOF_ADMIN
chgrp aowis-dvmdb "$config"
chmod 0640 "$config"
config_hash="$(sha256sum "$config" | cut -d ' ' -f 1)"
config_mode="$(stat -c '%a:%U:%G' "$config")"
printf '%s\n' 'persistent database fixture' > "$state/package-preservation-fixture"
chown aowis-dvmdb:aowis-dvmdb "$state/package-preservation-fixture"

apt-get install --yes --no-install-recommends "$upgrade_package"
test "$(sha256sum "$config" | cut -d ' ' -f 1)" = "$config_hash"
test "$(stat -c '%a:%U:%G' "$config")" = "$config_mode"
grep -Fxq 'persistent database fixture' "$state/package-preservation-fixture"

# After the upgrade, reopen the actual on-disk catalogue and verify the
# project created by the previous installed binary remains readable.
start_server
python3 - "$port" "$project_id" <<'PY'
import json
import sys
import urllib.request
port, project_id = sys.argv[1:]
with urllib.request.urlopen(f'http://127.0.0.1:{port}/dvmdb/v1/projects', timeout=5) as reply:
    projects = json.load(reply)['projects']
assert len(projects) == 1 and projects[0]['id'] == project_id, projects
assert projects[0]['name'] == 'Package smoke', projects
PY
cleanup_server

printf '\n== Remove/reinstall preserves config and state ==\n'
apt-get remove --yes aowis-server-dvmdb
test -f "$config"
test -f "$state/package-preservation-fixture"
apt-get install --yes --no-install-recommends "$upgrade_package"
test "$(sha256sum "$config" | cut -d ' ' -f 1)" = "$config_hash"
test "$(stat -c '%a:%U:%G' "$config")" = "$config_mode"
test -f "$state/package-preservation-fixture"

printf '\n== Purge removes config, never database ==\n'
apt-get purge --yes aowis-server-dvmdb
test ! -e "$config"
test -f "$state/package-preservation-fixture"
printf '\nDVMDB package installation, runtime and lifecycle qualification PASSED.\n'
