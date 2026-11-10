#!/usr/bin/env bash
set -euo pipefail

: "${DEB_REVISION:?DEB_REVISION must be set}"

scratch="$(mktemp -d /tmp/aowis-dvmdb-deb.XXXXXXXX)"
trap 'rm -rf -- "$scratch"' EXIT

# Do not copy any host build trees or generated artifacts into the source
# package. The checked-out source is mounted read-only by the Docker wrapper.
tar -C /source \
    --exclude='./.git' \
    --exclude='*/.git' \
    --exclude='*/__pycache__' \
    --exclude='./build' \
    --exclude='./build-*' \
    --exclude='*/build' \
    --exclude='*/build-*' \
    --exclude='./dist' \
    --exclude='./*.zip' \
    --exclude='./aowis-dvmdb-data' \
    --exclude='./.aowis-patch-delete.txt' \
    -cf - . | tar -C "$scratch" -xf -

cd "$scratch"
# The source tree may originate from an archive without executable file modes.
# Run the Bash entry point explicitly rather than relying on its mode bit.
bash ./compile_deb.sh --clean --revision "$DEB_REVISION"

shopt -s nullglob
packages=("$scratch"/dist/aowis-server-dvmdb_*.deb)
shopt -u nullglob

if (( ${#packages[@]} != 1 )); then
    printf 'Error: expected one Debian package, found %d\n' "${#packages[@]}" >&2
    exit 1
fi

package="${packages[0]}"
package_name="$(basename -- "$package")"
package_revision="$(dpkg-deb --field "$package" Version)"
if [[ "$package_revision" != *"+deb$DEB_REVISION" ]]; then
    printf 'Error: unexpected Debian package version: %s\n' "$package_revision" >&2
    exit 1
fi

# Check the actual package payload, not just whether dpkg-buildpackage exited 0.
package_contents="$(dpkg-deb --contents "$package")"
for required in \
    './usr/bin/aowis-server-dvmdb' \
    './usr/lib/systemd/system/aowis-server-dvmdb.service' \
    './etc/aowis/aowis-server-dvmdb.ini'; do
    if ! grep -Fq -- "$required" <<< "$package_contents"; then
        printf 'Error: package does not contain %s\n' "$required" >&2
        exit 1
    fi
done

install -m 0644 "$package" "/packages/$package_name"
printf 'Exported: %s\n' "/packages/$package_name"
