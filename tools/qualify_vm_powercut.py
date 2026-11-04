#!/usr/bin/env python3
"""Opt-in QEMU guest-cache-loss qualification on *disposable* virtual disks.

This is not an emulation of sudden power removal from the host SSD/controller.
Killing QEMU removes guest RAM and guest kernel cache; cache=none minimizes host
page-cache involvement, but the host kernel and actual device remain powered.
"""
import argparse
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

MOUNT = "/mnt/aowis-powercut"
DEVICE = "/dev/vdb"
WORKER = "/usr/local/bin/aowis-dvmdb-vm-powercut-worker"
STATE = re.compile(r"^STATE ([0-9]+) ([0-9a-f]{64})$")
ACK = re.compile(r"^ACK ([0-9]+) ([0-9a-f]{64})$")


def require_command(name):
    if not shutil.which(name):
        raise RuntimeError("required executable missing: " + name)


def execute(argv, timeout=90, **kwargs):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {argv[0]}: "
                           f"{result.stderr[-3000:]} {result.stdout[-1000:]}")
    return result.stdout


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
        connection.bind(("127.0.0.1", 0))
        return connection.getsockname()[1]


def marker(line, expression):
    result = expression.fullmatch(line.strip())
    if result is None:
        raise RuntimeError("unexpected durability worker output: " + repr(line))
    return int(result.group(1)), result.group(2)


def ssh_args(args, port):
    return ["ssh", "-T", "-i", str(args.ssh_key), "-p", str(port),
            "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
            "-o", "UserKnownHostsFile=" + str(args.known_hosts),
            "-o", "ConnectTimeout=4", "-o", "ServerAliveInterval=3",
            "-o", "ServerAliveCountMax=2",
            f"{args.guest_user}@127.0.0.1"]


def root_command(args, *words):
    return ([] if args.guest_user == "root" else ["sudo", "-n", "--"]) + list(words)


def remote(args, port, *words, timeout=90):
    import shlex
    return execute(ssh_args(args, port) + [shlex.join(root_command(args, *words))],
                   timeout=timeout)


def start_vm(args, overlay, disk, port, log):
    command = ["qemu-system-x86_64", "-machine", "q35,accel=" +
               ("kvm" if args.kvm else "tcg"), "-m", str(args.memory_mib),
               "-smp", "2", "-nographic", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-drive", f"file={overlay},if=virtio,format=qcow2,cache=none",
               "-drive", f"file={disk},if=virtio,format=raw,cache=none",
               "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22",
               "-device", "virtio-net-pci,netdev=net0"]
    if args.cloud_init_seed:
        command += ["-drive", f"file={args.cloud_init_seed.resolve()},if=virtio,format=raw,readonly=on"]
    return subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)


def stop_vm(process):
    if process is None or process.poll() is not None:
        return
    # SIGKILL of the whole VM, not of its guest database process.
    os.kill(process.pid, signal.SIGKILL)
    process.wait(timeout=20)


def await_ssh(args, port, process):
    deadline = time.monotonic() + args.boot_timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited during guest startup: {process.returncode}")
        try:
            if execute(ssh_args(args, port) + ["true"], timeout=7) == "":
                return
        except (RuntimeError, subprocess.TimeoutExpired):
            time.sleep(1)
    raise RuntimeError("guest SSH did not become ready; check VM serial.log and SSH key")


def assert_disposable_disk(args, port):
    # The boot image always occupies /dev/vda; the *newly created* raw image
    # is /dev/vdb. Reject unexpected device layouts before mkfs.
    result = remote(args, port, "lsblk", "-dn", "-o", "TYPE,SIZE", DEVICE).strip()
    if not result.startswith("disk "):
        raise RuntimeError("refusing to format unexpected guest data device: " + result)
    boot = remote(args, port, "lsblk", "-dn", "-o", "TYPE", "/dev/vda").strip()
    if boot != "disk":
        raise RuntimeError("guest boot device is not /dev/vda; refusing to format")
    mounted = remote(args, port, "findmnt", "-rn", "-o", "SOURCE", "--target", "/")
    if "/dev/vdb" in mounted:
        raise RuntimeError("guest root filesystem unexpectedly uses /dev/vdb")


def setup_mount(args, port, filesystem, first):
    assert_disposable_disk(args, port)
    remote(args, port, "mkdir", "-p", MOUNT)
    if first:
        if filesystem == "ext4":
            remote(args, port, "mkfs.ext4", "-F", DEVICE)
        else:
            remote(args, port, "mkfs.xfs", "-f", DEVICE)
    remote(args, port, "mount", "-t", filesystem, DEVICE, MOUNT)
    # The worker MUST run on the expected disk, never on the boot root.
    mounted = remote(args, port, "findmnt", "-rn", "-o", "SOURCE", "--target", MOUNT).strip()
    if mounted != DEVICE:
        raise RuntimeError("test mount is not the disposable data image: " + mounted)


def install_worker(args, port):
    target = f"{args.guest_user}@127.0.0.1:/tmp/aowis-powercut-worker"
    execute(["scp", "-q", "-i", str(args.ssh_key), "-P", str(port),
             "-o", "StrictHostKeyChecking=accept-new",
             "-o", "UserKnownHostsFile=" + str(args.known_hosts),
             str(args.worker), target])
    remote(args, port, "install", "-m", "0755", "/tmp/aowis-powercut-worker", WORKER)


def worker_state(args, port, mode):
    output = remote(args, port, WORKER, mode, MOUNT + "/repository", timeout=180)
    lines = [line for line in output.splitlines() if line.startswith("STATE ")]
    if len(lines) != 1:
        raise RuntimeError("missing worker state line: " + output[-1000:])
    return marker(lines[0], STATE)


def powercut_round(args, port, qemu, expected, round_index, report):
    import shlex
    command = ssh_args(args, port) + [shlex.join(root_command(
        args, WORKER, "writer", MOUNT + "/repository"))]
    with open(report / f"writer-{round_index:03d}.log", "wb") as err:
        writer = subprocess.Popen(command, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=err, bufsize=0)
        try:
            assert writer.stdout is not None and writer.stdin is not None
            ready, _, _ = select.select([writer.stdout], [], [], 25)
            if not ready:
                raise RuntimeError("writer produced no durable acknowledgement")
            line = writer.stdout.readline().decode("ascii", errors="replace")
            acknowledged, hash_value = marker(line, ACK)
            if acknowledged != expected + 1:
                raise RuntimeError("unexpected acknowledgement sequence")
            # Only after receiving the ACK do we request a further commit.
            writer.stdin.write(b"GO\n")
            writer.stdin.flush()
            time.sleep((2 + (round_index * 17) % 27) / 1000)
            stop_vm(qemu)
            return acknowledged, hash_value
        finally:
            try:
                writer.kill()
            except ProcessLookupError:
                pass
            try:
                writer.communicate(timeout=7)
            except subprocess.TimeoutExpired:
                writer.kill()
                writer.wait(timeout=7)


DELETION_BARRIERS = ("before-intent", "after-intent", "after-ref",
                     "after-drafts", "after-finish")


def deletion_case(round_index):
    # Alternate full branch removal and non-root truncation at each boundary.
    scenario = "branch" if round_index % 2 == 0 else "truncate"
    point = DELETION_BARRIERS[(round_index // 2) % len(DELETION_BARRIERS)]
    return scenario, point


def deletion_state(args, port, round_index, expected):
    output = remote(args, port, WORKER, "delete-verify",
                    MOUNT + f"/deletion-{round_index:03d}", expected, timeout=180)
    if f"DELETE-VERIFIED " not in output or expected not in output:
        raise RuntimeError("missing deletion recovery verification: " + output[-2000:])


def deletion_powercut_round(args, port, qemu, round_index, report):
    import shlex
    scenario, point = deletion_case(round_index)
    root = MOUNT + f"/deletion-{round_index:03d}"
    remote(args, port, WORKER, "delete-init", root, scenario, timeout=180)
    command = ssh_args(args, port) + [shlex.join(root_command(
        args, "env", "AOWIS_VM_DELETE_POINT=" + point,
        WORKER, "delete-writer", root))]
    with open(report / f"delete-writer-{round_index:03d}.log", "wb") as log:
        writer = subprocess.Popen(command, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=log, bufsize=0)
        try:
            assert writer.stdout is not None
            ready, _, _ = select.select([writer.stdout], [], [], 45)
            if not ready:
                raise RuntimeError(f"no deletion barrier {point} for {scenario}")
            line = writer.stdout.readline().decode("ascii", errors="replace").strip()
            if line != "BARRIER " + point:
                raise RuntimeError(f"unexpected deletion barrier: {line!r}, wanted {point}")
            # The guest is stopped INSIDE the requested deletion persistence
            # phase. No millisecond sleep or probabilistic crash window.
            stop_vm(qemu)
        finally:
            if writer.poll() is None:
                writer.kill()
            try:
                writer.communicate(timeout=7)
            except subprocess.TimeoutExpired:
                writer.kill()
                writer.wait(timeout=7)
    return {"round": round_index, "scenario": scenario, "barrier": point,
            "expected_recovery": "original" if point == "before-intent" else "deleted"}


def run_fs_deletions(args, workspace, filesystem):
    case = workspace / filesystem
    case.mkdir()
    disk = case / "disposable-data.raw"
    with disk.open("wb") as stream:
        stream.truncate(args.disk_mib * 1024 * 1024)
    overlay = case / "guest-overlay.qcow2"
    base_info = json.loads(execute(["qemu-img", "info", "--output=json", str(args.guest_image)]))
    if base_info["format"] not in ("qcow2", "raw"):
        raise RuntimeError("guest image format must be qcow2 or raw")
    execute(["qemu-img", "create", "-f", "qcow2", "-F", base_info["format"],
             "-b", str(args.guest_image.resolve()), str(overlay)])
    results = []
    with (case / "serial.log").open("wb") as log:
        qemu = None
        try:
            for round_index in range(args.rounds + 1):
                port = unused_port()
                qemu = start_vm(args, overlay, disk, port, log)
                await_ssh(args, port, qemu)
                if args.cloud_init_seed:
                    remote(args, port, "cloud-init", "status", "--wait", timeout=600)
                setup_mount(args, port, filesystem, round_index == 0)
                install_worker(args, port)
                # Recheck EVERY preceding completed deletion. This detects
                # regression in earlier durable state during later power cuts.
                for previous in results:
                    deletion_state(args, port, previous["round"], previous["expected_recovery"])
                if results:
                    results[-1]["verified_after_reboot"] = True
                if round_index == args.rounds:
                    break
                result = deletion_powercut_round(args, port, qemu, round_index, case)
                results.append(result)
                qemu = None
                print(f"{filesystem} deletion cut {round_index + 1}/{args.rounds}: "
                      f"{result['scenario']} at {result['barrier']}", flush=True)
        finally:
            stop_vm(qemu)
    return {"filesystem": filesystem, "workload": "deletions", "rounds": results,
            "result": "passed"}


def stress_powercut_round(args, port, qemu, round_index, report):
    import shlex
    command = ssh_args(args, port) + [shlex.join(root_command(
        args, WORKER, "stress-writer", MOUNT + "/stress"))]
    with open(report / f"stress-writer-{round_index:03d}.log", "wb") as log:
        writer = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=log, bufsize=0)
        try:
            assert writer.stdout is not None
            ready, _, _ = select.select([writer.stdout], [], [], 45)
            if not ready or writer.stdout.readline().decode("ascii", errors="replace").strip() != "STRESS-RUNNING":
                raise RuntimeError("deletion stress writer did not start")
            # Unlike the deterministic workload, these kills are deliberately
            # randomized relative to a continuous stream of real operations.
            time.sleep((37 + (round_index * 43) % 211) / 1000)
            if writer.poll() is not None:
                raise RuntimeError("deletion stress worker exited before VM cut")
            stop_vm(qemu)
        finally:
            if writer.poll() is None:
                writer.kill()
            try:
                writer.communicate(timeout=7)
            except subprocess.TimeoutExpired:
                writer.kill()
                writer.wait(timeout=7)
    return {"round": round_index, "crash_delay_ms": 37 + (round_index * 43) % 211}


def run_fs_deletion_stress(args, workspace, filesystem):
    case = workspace / filesystem
    case.mkdir()
    disk = case / "disposable-data.raw"
    with disk.open("wb") as stream:
        stream.truncate(args.disk_mib * 1024 * 1024)
    overlay = case / "guest-overlay.qcow2"
    base_info = json.loads(execute(["qemu-img", "info", "--output=json", str(args.guest_image)]))
    if base_info["format"] not in ("qcow2", "raw"):
        raise RuntimeError("guest image format must be qcow2 or raw")
    execute(["qemu-img", "create", "-f", "qcow2", "-F", base_info["format"],
             "-b", str(args.guest_image.resolve()), str(overlay)])
    results = []
    with (case / "serial.log").open("wb") as log:
        qemu = None
        try:
            for round_index in range(args.rounds + 1):
                port = unused_port()
                qemu = start_vm(args, overlay, disk, port, log)
                await_ssh(args, port, qemu)
                if args.cloud_init_seed:
                    remote(args, port, "cloud-init", "status", "--wait", timeout=600)
                setup_mount(args, port, filesystem, round_index == 0)
                install_worker(args, port)
                if round_index == 0:
                    remote(args, port, WORKER, "stress-init", MOUNT + "/stress", timeout=180)
                else:
                    result = remote(args, port, WORKER, "stress-verify",
                                    MOUNT + "/stress", timeout=180)
                    if "STRESS-VERIFIED " not in result:
                        raise RuntimeError("stress recovery returned no verification marker")
                    results[-1]["verified_after_reboot"] = True
                if round_index == args.rounds:
                    break
                results.append(stress_powercut_round(args, port, qemu, round_index, case))
                qemu = None
                print(f"{filesystem} continuous deletion cut {round_index + 1}/{args.rounds}",
                      flush=True)
        finally:
            stop_vm(qemu)
    return {"filesystem": filesystem, "workload": "deletion-stress",
            "rounds": results, "result": "passed"}


def run_fs(args, workspace, filesystem):
    case = workspace / filesystem
    case.mkdir()
    disk = case / "disposable-data.raw"
    with disk.open("wb") as stream:
        stream.truncate(args.disk_mib * 1024 * 1024)
    overlay = case / "guest-overlay.qcow2"
    base_info = json.loads(execute(["qemu-img", "info", "--output=json", str(args.guest_image)]))
    base_format = base_info["format"]
    if base_format not in ("qcow2", "raw"):
        raise RuntimeError("guest image format must be qcow2 or raw")
    execute(["qemu-img", "create", "-f", "qcow2", "-F", base_format,
             "-b", str(args.guest_image.resolve()), str(overlay)])
    results = []
    acknowledged = 0
    with (case / "serial.log").open("wb") as log:
        qemu = None
        try:
            for round_index in range(args.rounds + 1):
                port = unused_port()  # Do not reuse the killed VM's listen socket.
                qemu = start_vm(args, overlay, disk, port, log)
                await_ssh(args, port, qemu)
                if args.cloud_init_seed:
                    remote(args, port, "cloud-init", "status", "--wait", timeout=600)
                setup_mount(args, port, filesystem, round_index == 0)
                # The worker lives on the guest OS overlay, NOT the durable
                # data disk under test. QEMU SIGKILL can discard guest-cached
                # writes that installed the binary on the previous boot.
                # Stage it again before *every* recovery verification, so an
                # unrelated missing test executable cannot obscure the result.
                install_worker(args, port)
                if round_index == 0:
                    state, head = worker_state(args, port, "init")
                    if state != 0:
                        raise RuntimeError("initial repository state is nonzero")
                else:
                    state, head = worker_state(args, port, "verify")
                    if not (acknowledged <= state <= acknowledged + 1):
                        raise RuntimeError(f"lost acknowledged commit or jumped ahead: "
                                           f"acked={acknowledged}, persisted={state}")
                    results[-1]["recovered_sequence"] = state
                    results[-1]["recovered_head"] = head
                    acknowledged = state
                if round_index == args.rounds:
                    break
                accepted, accepted_head = powercut_round(
                    args, port, qemu, acknowledged, round_index, case)
                results.append({"round": round_index, "acknowledged": accepted,
                                "acknowledged_head": accepted_head})
                acknowledged = accepted
                qemu = None
                print(f"{filesystem} cut {round_index + 1}/{args.rounds}: acknowledged={accepted}",
                      flush=True)
        finally:
            stop_vm(qemu)
    return {"filesystem": filesystem, "rounds": results,
            "final_sequence": acknowledged, "result": "passed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest-image", type=Path, required=True,
                        help="existing bootable x86_64 Linux qcow2/raw image with SSH and tools")
    parser.add_argument("--ssh-key", type=Path, required=True, help="private key authorized in guest")
    parser.add_argument("--guest-user", default="root", help="SSH user (otherwise passwordless sudo)")
    parser.add_argument("--cloud-init-seed", type=Path,
                        help="optional cloud-localds seed.img (attached read-only as /dev/vdc)")
    parser.add_argument("--worker", type=Path, required=True, help="guest-compatible Release worker binary")
    parser.add_argument("--report-json", type=Path, help="write final verified JSON report outside disposable workspace")
    parser.add_argument("--filesystems", nargs="+", choices=["ext4", "xfs"], default=["ext4", "xfs"])
    parser.add_argument("--workload", choices=("commits", "deletions", "deletion-stress"), default="commits")
    parser.add_argument("--rounds", type=int, default=16)
    parser.add_argument("--disk-mib", type=int, default=512)
    parser.add_argument("--memory-mib", type=int, default=1024)
    parser.add_argument("--boot-timeout", type=int, default=150)
    parser.add_argument("--work-root", type=Path, help="directory for disposable guest overlays and logs")
    parser.add_argument("--kvm", action="store_true", help="use /dev/kvm if available")
    args = parser.parse_args()
    if args.rounds < 1 or args.disk_mib < 256 or args.memory_mib < 512:
        parser.error("rounds >= 1, disk-mib >= 256, memory-mib >= 512 required")
    for path in (args.guest_image, args.ssh_key, args.worker, args.cloud_init_seed):
        if path is None:
            continue
        if not path.is_file():
            parser.error("required input file does not exist: " + str(path))
    if args.kvm and not os.access("/dev/kvm", os.R_OK | os.W_OK):
        parser.error("--kvm requires accessible /dev/kvm")
    for name in ("qemu-system-x86_64", "qemu-img", "ssh", "scp"):
        require_command(name)
    if args.work_root:
        args.work_root.mkdir(parents=True, exist_ok=True)
    workspace = Path(tempfile.mkdtemp(prefix="aowis-powercut-vm-", dir=args.work_root))
    args.known_hosts = workspace / "known_hosts"
    try:
        runner = {"commits": run_fs, "deletions": run_fs_deletions,
                  "deletion-stress": run_fs_deletion_stress}[args.workload]
        results = [runner(args, workspace, filesystem) for filesystem in args.filesystems]
        (workspace / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        if args.report_json:
            args.report_json.write_text(json.dumps(results, indent=2) + "\n")
        print("Power-cut guest-cache-loss qualification passed:", workspace, flush=True)
    except Exception:
        print("Power-cut qualification FAILED; preserved evidence:", workspace,
              file=sys.stderr)
        raise
    # Keep small summary only; the disposable OS overlays and data images have
    # no value after a successful qualification and can be many gigabytes.
    print(json.dumps(results, indent=2))
    shutil.rmtree(workspace)


if __name__ == "__main__":
    main()
