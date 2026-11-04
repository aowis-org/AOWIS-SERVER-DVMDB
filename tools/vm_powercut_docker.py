#!/usr/bin/env python3
"""Build and run the QEMU power-cut qualification from its Docker container.

The source is mounted read-only at /src. /cache and /results contain only
qualification artifacts; no host block devices or production repositories.
"""
import argparse
import fcntl
import hashlib
import json
import os
import pwd
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.request import urlopen

SOURCE = Path("/src")
CACHE = Path("/cache")
RESULTS = Path("/results")
IMAGE_NAME = "debian-12-genericcloud-amd64.qcow2"
DEBIAN_CLOUD = "https://cloudfront.debian.net/cdimage/cloud/bookworm/latest"


def run(command, **kwargs):
    print("+", " ".join(str(value) for value in command), flush=True)
    subprocess.run([str(value) for value in command], check=True, **kwargs)


def require_container_identity():
    """Fail before costly VM setup if Docker's numeric UID has no NSS identity."""
    uid = os.geteuid()
    try:
        user = pwd.getpwuid(uid)
    except KeyError as error:
        raise RuntimeError(
            f"Docker UID {uid} is not present in the container passwd database; "
            "rebuild the VM qualification image using "
            "./tools/prepare_vm_powercut_docker.sh"
        ) from error
    if not user.pw_name:
        raise RuntimeError(f"Docker UID {uid} has an empty account name")


def checksum_for(text, name=IMAGE_NAME):
    for line in text.splitlines():
        found = re.fullmatch(r"([0-9a-fA-F]{128})\s+\*?(.+)", line.strip())
        if found and found.group(2).lstrip("./") == name:
            return found.group(1).lower()
    raise RuntimeError("official Debian SHA512SUMS does not list " + name)


def sha512(path):
    digest = hashlib.sha512()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download_guest(cache, *, refresh=False, base_url=DEBIAN_CLOUD):
    destination = cache / IMAGE_NAME
    digest_file = cache / (IMAGE_NAME + ".sha512")
    if not refresh and destination.is_file() and digest_file.is_file():
        expected = digest_file.read_text(encoding="ascii").strip()
        if re.fullmatch(r"[0-9a-f]{128}", expected) and sha512(destination) == expected:
            print("Using verified cached Debian 12 cloud image.", flush=True)
            return destination
        raise RuntimeError("cached guest image checksum mismatch; inspect cache, then use --refresh-image")

    print("Retrieving official Debian cloud image checksum...", flush=True)
    with urlopen(base_url + "/SHA512SUMS", timeout=60) as response:
        expected = checksum_for(response.read().decode("utf-8"))
    fd, temporary_name = tempfile.mkstemp(prefix="aowis-guest-", suffix=".part", dir=cache)
    temp = Path(temporary_name)
    try:
        digest = hashlib.sha512()
        with os.fdopen(fd, "wb") as output:
            print("Downloading official Debian 12 guest image (first run only)...", flush=True)
            with urlopen(base_url + "/" + IMAGE_NAME, timeout=120) as response:
                while block := response.read(1024 * 1024):
                    digest.update(block)
                    output.write(block)
            output.flush()
            os.fsync(output.fileno())
        if digest.hexdigest() != expected:
            raise RuntimeError("Debian guest image failed its official SHA-512 verification")
        os.replace(temp, destination)
        digest_file.write_text(expected + "\n", encoding="ascii")
        print("Debian cloud image SHA-512 verified.", flush=True)
        return destination
    finally:
        temp.unlink(missing_ok=True)


def build_worker(cache, source=SOURCE):
    build = cache / "debian12-release-build"
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / "worker-build.lock").open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        run(["cmake", "-S", source, "-B", build, "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
             "-DAOWIS_DVMDB_WITH_MODEL_CODEC=OFF", "-DAOWIS_DVMDB_WITH_HTTP_SERVER=OFF",
             "-DAOWIS_DVMDB_VM_DELETION_BARRIERS=ON"])
        run(["cmake", "--build", build, "--target", "aowis-dvmdb-vm-powercut-worker",
             "--parallel", "2"])
    worker = build / "aowis-dvmdb-vm-powercut-worker"
    if not worker.is_file():
        raise RuntimeError("CMake did not produce a guest worker: " + str(worker))
    return worker


def make_cloud_init(directory, key):
    public = key.with_suffix(".pub").read_text(encoding="ascii").strip()
    if not public.startswith("ssh-ed25519 "):
        raise RuntimeError("unexpected SSH public key format")
    user_data = directory / "user-data"
    user_data.write_text("""#cloud-config
hostname: aowis-durability-vm
ssh_pwauth: false
users:
  - name: aowis-test
    shell: /bin/bash
    lock_passwd: true
    sudo: 'ALL=(ALL) NOPASSWD:ALL'
    ssh_authorized_keys:
      - """ + public + """
packages:
  - e2fsprogs
  - xfsprogs
  - util-linux
  - openssh-server
runcmd:
  - [systemctl, enable, --now, ssh]
""", encoding="utf-8")
    (directory / "meta-data").write_text(
        "instance-id: aowis-disposable-" + directory.name + "\n"
        "local-hostname: aowis-durability-vm\n", encoding="utf-8")
    seed = directory / "cloud-init.img"
    run(["cloud-localds", seed, user_data, directory / "meta-data"])
    return seed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload", choices=("commits", "deletions", "deletion-stress"), default="commits",
                        help="existing commit qualification or deletion crash barriers")
    parser.add_argument("--rounds", type=int, default=4, help="power cuts per filesystem (default: 4)")
    parser.add_argument("--filesystems", nargs="+", choices=("ext4", "xfs"),
                        default=["ext4", "xfs"])
    parser.add_argument("--disk-mib", type=int, default=512)
    parser.add_argument("--memory-mib", type=int, default=1024)
    parser.add_argument("--boot-timeout", type=int, default=600)
    parser.add_argument("--refresh-image", action="store_true",
                        help="download a new official guest image and checksum")
    args = parser.parse_args(argv)
    if args.rounds < 1 or args.disk_mib < 256 or args.memory_mib < 512 or args.boot_timeout < 30:
        parser.error("rounds >= 1, disk-mib >= 256, memory-mib >= 512, boot-timeout >= 30")
    require_container_identity()
    CACHE.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    image = download_guest(CACHE, refresh=args.refresh_image)
    worker = build_worker(CACHE)
    run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=RESULTS))
    key = run_dir / "guest-key"
    report = run_dir / "report.json"
    try:
        run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key])
        seed = make_cloud_init(run_dir, key)
        kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
        command = [sys.executable, SOURCE / "tools/qualify_vm_powercut.py",
                   "--guest-image", image, "--cloud-init-seed", seed,
                   "--ssh-key", key, "--guest-user", "aowis-test",
                   "--worker", worker, "--workload", args.workload, "--rounds", args.rounds,
                   "--disk-mib", args.disk_mib, "--memory-mib", args.memory_mib,
                   "--boot-timeout", args.boot_timeout, "--filesystems", *args.filesystems,
                   "--work-root", run_dir, "--report-json", report]
        if kvm:
            command.append("--kvm")
        print("QEMU: " + ("KVM acceleration" if kvm else "software emulation"), flush=True)
        log_path = run_dir / "qualification.log"
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(value) for value in command],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, bufsize=1)
            assert process.stdout is not None
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
                log.flush()
            code = process.wait()
        if code:
            raise RuntimeError(f"QEMU qualification exited with status {code}")
        # The guest credentials are disposable. Keep only the test report and logs.
        for name in ("guest-key", "guest-key.pub", "cloud-init.img", "user-data", "meta-data"):
            (run_dir / name).unlink(missing_ok=True)
        print(f"PASS: QEMU ext4/XFS report: {report}", flush=True)
    except Exception:
        print(f"FAILED: retained isolated evidence and guest credentials at {run_dir}",
              file=sys.stderr)
        raise


if __name__ == "__main__":
    main()
