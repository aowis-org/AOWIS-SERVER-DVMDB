#!/usr/bin/env bash
set -euo pipefail

project_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_directory="$project_directory/build-deb"
dist_directory="$project_directory/dist"
install_dependencies=false
clean_build=false
debian_revision="${DEB_REVISION:-1}"

print_usage()
{
    cat <<EOF_USAGE
Usage: ./compile_deb.sh [options]

Build a Debian binary package in dist/.

Options:
  --install-dependencies  Install the Debian build dependencies with apt-get.
  --revision <number>     Debian package revision. Default: $debian_revision
  --clean                 Remove previous Debian build output before building.
  -h, --help              Show this help.

Examples:
  ./compile_deb.sh --install-dependencies
  ./compile_deb.sh --clean --revision 2
EOF_USAGE
}

is_debian_family()
{
    if [[ ! -r /etc/os-release ]]; then
        return 1
    fi

    local ID=""
    local ID_LIKE=""
    # shellcheck disable=SC1091
    source /etc/os-release

    if [[ "${ID:-}" == "debian" ]]; then
        return 0
    fi

    [[ " ${ID_LIKE:-} " == *" debian "* ]]
}

require_debian()
{
    if is_debian_family; then
        return
    fi

    echo "Error: compile_deb.sh must run on Debian or a Debian-based distribution." >&2
    exit 1
}

install_debian_dependencies()
{
    local packages=(
        build-essential
        cmake
        ninja-build
        debhelper
        dpkg-dev
        pkg-config
        qt6-base-dev
        libssl-dev
        python3
    )

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "Error: apt-get is unavailable." >&2
        exit 1
    fi

    if (( EUID == 0 )); then
        apt-get update
        apt-get install -y "${packages[@]}"
        return
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        echo "Error: dependency installation requires root privileges or sudo." >&2
        exit 1
    fi

    sudo apt-get update
    sudo apt-get install -y "${packages[@]}"
}

require_build_tools()
{
    local missing_tools=()
    local tool=""

    for tool in cmake c++ ninja dh dpkg-buildpackage dpkg-deb tar; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing_tools+=("$tool")
        fi
    done

    if (( ${#missing_tools[@]} == 0 )); then
        return
    fi

    echo "Error: missing Debian package build tools: ${missing_tools[*]}" >&2
    echo "Run: ./compile_deb.sh --install-dependencies" >&2
    exit 1
}

read_project_version()
{
    python3 - "$project_directory/CMakeLists.txt" <<'PY_VERSION'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding='utf-8')
match = re.search(
    r'\bproject\s*\([^)]*\bVERSION\s+([0-9]+(?:\.[0-9]+){1,3})\b',
    source,
    re.IGNORECASE,
)
if match is None:
    print('Error: unable to read the project version from CMakeLists.txt.', file=sys.stderr)
    sys.exit(1)
print(match.group(1))
PY_VERSION
}

while (( $# > 0 )); do
    case "$1" in
        --install-dependencies)
            install_dependencies=true
            shift
            ;;
        --revision)
            if (( $# < 2 )) || [[ -z "$2" ]]; then
                echo "Error: --revision requires a value." >&2
                exit 2
            fi
            debian_revision="$2"
            shift 2
            ;;
        --clean)
            clean_build=true
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Error: unknown option: $1" >&2
            print_usage >&2
            exit 2
            ;;
    esac
done

require_debian

if [[ ! "$debian_revision" =~ ^[0-9][0-9A-Za-z.+~]*$ ]]; then
    echo "Error: invalid Debian revision: $debian_revision" >&2
    exit 2
fi

if [[ "$install_dependencies" == true ]]; then
    install_debian_dependencies
fi

require_build_tools

if [[ ! -f "$project_directory/external/AOWIS-SERVER-MODEL/CMakeLists.txt" ]]; then
    printf '%s\n' 'Error: missing MODEL submodule; run ./git_submodule_init.sh first.' >&2
    exit 1
fi

project_version="$(read_project_version)"
package_version="$project_version+deb$debian_revision"
source_directory="$build_directory/aowis-server-dvmdb-$project_version"

if [[ "$clean_build" == true ]]; then
    rm -rf -- "$build_directory"
fi

rm -rf -- "$source_directory"
mkdir -p "$source_directory" "$dist_directory"

tar \
    --exclude='./.git' \
    --exclude='./build' \
    --exclude='./build-*' \
    --exclude='*/build' \
    --exclude='*/build-*' \
    --exclude='./build-deb' \
    --exclude='./dist' \
    --exclude='./aowis-dvmdb-data' \
    --exclude='./*.zip' \
    --exclude='*/.git' \
    --exclude='*/__pycache__' \
    -C "$project_directory" -cf - . | tar -C "$source_directory" -xf -

cat > "$source_directory/debian/changelog" <<EOF_CHANGELOG
aowis-server-dvmdb ($package_version) stable; urgency=medium

  * Automated Debian package build.

 -- AOWIS Project <aowis@aowis.org>  $(date -R)
EOF_CHANGELOG

export DEB_BUILD_OPTIONS="parallel=$(nproc 2>/dev/null || printf '1') nocheck"
(
    cd "$source_directory"
    dpkg-buildpackage -b -us -uc
)

shopt -s nullglob
built_packages=("$build_directory"/aowis-server-dvmdb_"$package_version"_*.deb)
shopt -u nullglob

if (( ${#built_packages[@]} != 1 )); then
    echo "Error: expected exactly one package, found ${#built_packages[@]}." >&2
    exit 1
fi

package_path="$dist_directory/$(basename -- "${built_packages[0]}")"
install -m 0644 "${built_packages[0]}" "$package_path"

echo
echo "Debian package created:"
echo "  $package_path"
echo
echo "Install it with:"
echo "  sudo apt install ./${package_path#"$project_directory/"}"
echo
dpkg-deb --info "$package_path"
