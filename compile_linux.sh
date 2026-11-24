#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
    if [[ $# -gt 0 ]]; then
        printf '%s\n' "$1" >&2
    fi
    printf 'Usage: %s [--build-only | [--listen-address ADDRESS[:PORT]] [--listen-port PORT] [--data-root DIR]]\n' \
        "${0##*/}" >&2
    printf '  ADDRESS: numeric IPv4 or IPv6 address, e.g. 0.0.0.0, 127.0.0.1, ::, ::1.\n' >&2
    printf '  With a port: 0.0.0.0:8755, or [::]:8755 for IPv6. PORT: 1..65535.\n' >&2
    printf '  Without these options the server uses aowis-server-dvmdb.ini or 127.0.0.1:8755.\n' >&2
    printf '  DIR: project storage root (relative to the current directory; ~/ is your home).\n' >&2
    printf '  Without it: [storage] root of the ini, ./aowis-dvmdb-data of the checkout if it\n' >&2
    printf '  exists, else ~/.local/share/aowis-dvmdb.\n' >&2
    exit 2
}

# A port: decimal 1..65535 (no sign, no leading spaces; 10# avoids octal).
valid_port() {
    [[ "$1" =~ ^[0-9]{1,5}$ ]] && (( 10#$1 >= 1 && 10#$1 <= 65535 ))
}

# All options are parsed before CMake runs: a mistyped option never builds or
# starts anything.
build_only=false
listen_address=''
listen_port=''
address_given=false
port_given=false
data_root=''
data_root_given=false
while [[ $# -gt 0 ]]; do
    option="$1"
    shift
    value=''
    case "$option" in
        --listen-address=*|--listen-port=*|--data-root=*)
            value="${option#*=}"
            option="${option%%=*}"
            ;;
        --listen-address|--listen-port|--data-root)
            [[ $# -gt 0 ]] || usage "Missing value for $option."
            value="$1"
            shift
            ;;
    esac
    case "$option" in
        --build-only)
            [[ "$build_only" == false ]] || usage 'Option --build-only given twice.'
            build_only=true
            ;;
        --listen-address)
            [[ "$address_given" == false ]] || usage 'Option --listen-address given twice.'
            address_given=true
            address_port=''
            if [[ "$value" =~ ^\[([^]]*)\](:(.*))?$ ]]; then
                # [IPv6] or [IPv6]:PORT
                listen_address="${BASH_REMATCH[1]}"
                if [[ -n "${BASH_REMATCH[2]}" ]]; then
                    address_port="${BASH_REMATCH[3]}"
                    [[ -n "$address_port" ]] || usage "Missing port in --listen-address $value."
                fi
            elif [[ "$value" == *:*:* ]]; then
                # Two or more colons: a bare IPv6 address, never with a port.
                listen_address="$value"
            elif [[ "$value" == *:* ]]; then
                # IPv4:PORT
                listen_address="${value%%:*}"
                address_port="${value#*:}"
                [[ -n "$address_port" ]] || usage "Missing port in --listen-address $value."
            else
                listen_address="$value"
            fi
            [[ -n "$listen_address" ]] || usage "Missing address in --listen-address $value."
            # Numeric addresses only (the server refuses host names); this
            # rejects typos such as 'localhost' before anything is built.
            [[ "$listen_address" =~ ^[0-9A-Fa-f.:]+$ ]] \
                || usage "Invalid listen address '$listen_address': use a numeric IPv4 or IPv6 address."
            if [[ -n "$address_port" ]]; then
                valid_port "$address_port" || usage "Invalid listen port '$address_port' (1..65535)."
                [[ "$port_given" == false ]] || usage 'The listen port is given twice.'
                port_given=true
                listen_port="$address_port"
            fi
            ;;
        --listen-port)
            [[ "$port_given" == false ]] || usage 'The listen port is given twice.'
            valid_port "$value" || usage "Invalid listen port '$value' (1..65535)."
            port_given=true
            listen_port="$value"
            ;;
        --data-root)
            [[ "$data_root_given" == false ]] || usage 'Option --data-root given twice.'
            [[ -n "$value" ]] || usage 'Missing directory for --data-root.'
            data_root_given=true
            # ~ at the start means the home directory even where the shell did
            # not expand it (--data-root=~/...).
            if [[ "$value" == '~' ]]; then
                value="$HOME"
            elif [[ "$value" == '~/'* ]]; then
                value="$HOME/${value#'~/'}"
            fi
            # The server runs in the project directory: resolve a relative
            # directory against the caller's current directory now.
            if [[ "$value" != /* ]]; then
                value="$PWD/$value"
            fi
            data_root="$value"
            ;;
        *)
            usage "Unknown option: $option"
            ;;
    esac
done
if [[ "$build_only" == true && ( "$address_given" == true || "$port_given" == true ) ]]; then
    usage '--build-only starts no server: it takes no listen address or port.'
fi
if [[ "$build_only" == true && "$data_root_given" == true ]]; then
    usage '--build-only starts no server: it takes no data root.'
fi
# The server's own options (aowis-server-dvmdb --bind/--port/--data-root); 10#
# drops leading zeros of the port.
server_arguments=()
if [[ -n "$listen_address" ]]; then
    server_arguments+=(--bind "$listen_address")
fi
if [[ -n "$listen_port" ]]; then
    server_arguments+=(--port "$((10#$listen_port))")
fi
if [[ -n "$data_root" ]]; then
    server_arguments+=(--data-root "$data_root")
fi

build_dir="$project_dir/build-linux"
server="$build_dir/aowis-server-dvmdb"

# Explicit success gates matter when build-linux still contains a usable binary
# from an earlier build. Never fall through to that old server on a failure.
if cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release; then
    :
else
    status=$?
    printf 'DVMDB configuration failed (exit %d); server NOT started.\n' "$status" >&2
    exit "$status"
fi

# Build only the server and its transitive library dependencies. Building the
# default ALL target also compiles unrelated command-line tools and benchmarks.
# An explicit target build fails when the HTTP server target is unavailable;
# this also prevents an old executable from being launched after reconfiguration.
if cmake --build "$build_dir" --target aowis-server-dvmdb --parallel; then
    :
else
    status=$?
    printf 'DVMDB server build failed (exit %d); server NOT started.\n' "$status" >&2
    exit "$status"
fi

if [[ ! -f "$server" || ! -x "$server" ]]; then
    printf 'DVMDB build reported success, but the server executable is missing or not executable: %s\n' "$server" >&2
    exit 1
fi

printf 'AOWIS DVMDB Release server built in build-linux.\n'
if [[ "$build_only" == true ]]; then
    exit 0
fi

# Use the checked-out project's absolute binary, never ./build-linux relative
# to a different caller's current working directory.
printf 'Starting freshly verified DVMDB server: %s\n' "$server"
if [[ ${#server_arguments[@]} -gt 0 ]]; then
    printf 'Server options: %s\n' "${server_arguments[*]}"
fi
cd -- "$project_dir"
exec "$server" ${server_arguments[@]+"${server_arguments[@]}"}
