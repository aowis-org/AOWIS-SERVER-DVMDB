# Debian packages: Docker build and installation qualification

DVMDB supports the Debian 13 (**trixie**) packaging workflow used by the AOWIS
MAP server. `debian/` contains the normal `debhelper` packaging metadata and
`compile_deb.sh` runs the native Debian builder. The preferred entry point on
an Arch/Linux development machine is the Docker wrapper: it does **not**
install Qt, OpenSSL or Debian packaging tools on the host.

## Build on the development host

Prerequisites: Docker and access to its daemon, network access to Debian's
package repositories on first use, and an initialized `external/AOWIS-SERVER-MODEL`
submodule (`./git_submodule_init.sh`). Docker access is privileged: only run
trusted source code. From the DVMDB repository root:

```bash
./compile_deb_docker.sh
```

Output: `dist/aowis-server-dvmdb_0.1.0+deb1_<arch>.deb` (the version comes
from the current `CMakeLists.txt`). For a subsequent Debian revision:

```bash
./compile_deb_docker.sh --revision 2
```

To qualify the *installed* package, not merely compile it:

```bash
./compile_deb_docker.sh --test
```

This also builds revision 2 and starts a disposable Debian 13 container to
check: installation and declared runtime dependencies; packaged systemd unit;
service account, configuration and data permissions; actual HTTP `/health`,
`/ready`, and `/projects`, including creating a real project as the unprivileged
service user; upgrade to the next Debian revision; remove/reinstall; and purge. It checks the administrator's
modified configuration, the created project, and a storage-root fixture survive
upgrades and removal, and **database data survives package purge**. The test suppresses
automatic service starts during apt transactions (Docker has no systemd PID 1)
and runs the installed server directly. It does not qualify a full systemd
boot, a reverse proxy, or crash recovery. The default test HTTP port is
**18755** on host loopback with the default host network. Make sure this
port is free on the development machine when using `--test`.

`compile_deb_docker.sh` builds a reusable Debian toolchain image. It mounts
this source checkout read-only, compiles as the host user's UID/GID in a
throwaway directory, and exports only the resulting `.deb` into `dist/`.
The wrapper checks that the produced package is new and contains the server,
unit and INI. It never starts or modifies a host DVMDB server.

### Docker networking

The Docker wrapper defaults to **host networking** for all three phases:
toolchain image creation (`docker build`), compilation (`docker run`), and
installation qualification (`docker run --test`). This avoids DNS resolution
failures in Docker's bridge network when retrieving Debian dependencies.

```bash
./compile_deb_docker.sh --test
```

**Warning:** the qualification server binds `127.0.0.1:18755` in the host
network namespace. Ensure the port is free; the test does not modify host
files, installed services, or databases. The package build itself does not
start the server. To explicitly use the isolated Docker bridge network instead:

```bash
./compile_deb_docker.sh --test --build-network bridge --test-network bridge
```

`--build-network` controls both toolchain image creation and the package-build
container. `--test-network` controls the separate qualification container.

## Native build on Debian

```bash
./compile_deb.sh --install-dependencies
# Alternatively, if dependencies are already installed:
./compile_deb.sh --clean --revision 2
```

Native Debian builds use `dpkg-buildpackage`, `debhelper`, Ninja, CMake,
Qt 6 Core, OpenSSL development files and Python 3 for the MODEL codec
checks. The packaging target compiles **only `aowis-server-dvmdb` and its
transitive dependencies**; it does not compile unrelated tools, benchmarks or
the test suite. Run `./compile_linux_tests.sh` separately for CTest coverage.

## Installed layout and operation

| Item | Location |
|---|---|
| Server binary | `/usr/bin/aowis-server-dvmdb` |
| systemd unit | `/usr/lib/systemd/system/aowis-server-dvmdb.service` |
| INI (dpkg conffile) | `/etc/aowis/aowis-server-dvmdb.ini` |
| Persistent database | `/var/lib/aowis-dvmdb/` |
| Service user/group | `aowis-dvmdb:aowis-dvmdb` |

The unit starts with the packaged INI and listens on **`0.0.0.0:8755`**
(all IPv4 interfaces) by default; the standalone server without the deployment
INI still defaults to `127.0.0.1:8755`. The unit uses systemd
`StateDirectory=aowis-dvmdb` with mode `0700`. `ProtectSystem=strict`
and other hardening options are inherited from the project's deployment unit.
The **package-specific** unit calls `/usr/bin/`, not the `/usr/local/bin/`
path from the manual-install example in `deploy/`. This avoids silently
changing existing manually installed deployments.

Install, inspect, or restart a package on an actual Debian 13 system:

```bash
sudo apt install ./dist/aowis-server-dvmdb_0.1.0+deb1_amd64.deb  # example filename
sudo systemctl status aowis-server-dvmdb
sudo journalctl -u aowis-server-dvmdb -f
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
```

Do not install this package over an existing manually deployed service and
data root without stopping the old process and planning the transition: only
one process can own a root (`catalogue.lock`), and the default TCP port is
shared. The `.deb` does not delete, migrate or reinitialize existing database
files. The INI is a dpkg conffile, so local changes are preserved on regular
upgrade and package removal; **purge removes the INI but preserves
`/var/lib/aowis-dvmdb/`**. The service account is retained for ownership
stability. To override the unit, use `sudo systemctl edit aowis-server-dvmdb`.

**Security:** The systemd service's `0.0.0.0` default exposes TCP port
**8755 on every IPv4 network interface**. DVMDB currently has no HTTP
authentication/authorization or built-in HTTPS. Restrict inbound access with a
firewall or trusted VPN *before* starting the service. To restore loopback-only
binding, set `bind=127.0.0.1` under `[server]` in
`/etc/aowis/aowis-server-dvmdb.ini` and restart the service. TLS termination
alone does not authorize users. On an existing installation, dpkg may preserve
a locally modified INI on upgrade, so check its `bind=` value explicitly.
See [service configuration](native-database-service.md) and
[security design](security-permissions-design.md).
