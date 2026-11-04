#!/usr/bin/env python3
"""Offline VM-runner orchestration checks; QEMU, guest, and disks are mocked."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch


def load_runner(path):
    module_spec = importlib.util.spec_from_file_location("aowis_powercut_runner", path)
    runner = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(runner)
    return runner


def check_worker_installed_after_each_boot(runner):
    # The test deliberately makes the OS-installed binary disappear on *every*
    # QEMU reboot. Only the independent repository's simulated data survives.
    # This reproduces the real failure in which the first post-cut verification
    # failed before touching the repository because its helper was missing.
    with tempfile.TemporaryDirectory(prefix="aowis-vm-runner-tests-") as location:
        root = Path(location)
        image = root / "guest.qcow2"
        image.write_bytes(b"mock cloud-image")
        args = SimpleNamespace(guest_image=image, disk_mib=256, rounds=2,
                               cloud_init_seed=None)
        installed = False
        install_count = 0
        boots = 0
        mounted = []
        persisted_sequence = 0
        zero_hash = "0" * 64

        def fake_execute(command, **_kwargs):
            if command[:3] == ["qemu-img", "info", "--output=json"]:
                return json.dumps({"format": "qcow2"})
            if command[:3] == ["qemu-img", "create", "-f"]:
                return ""
            raise AssertionError("unexpected executable in mocked runner: " + repr(command))

        def fake_start(*_args):
            nonlocal installed, boots
            installed = False  # guest OS cache/installation lost across power cut
            boots += 1
            return object()

        def fake_mount(_args, _port, _filesystem, first):
            mounted.append(first)

        def fake_install(_args, _port):
            nonlocal installed, install_count
            assert not installed, "installer unexpectedly reused the preceding boot"
            installed = True
            install_count += 1

        def fake_state(_args, _port, mode):
            nonlocal persisted_sequence
            assert installed, "worker missing after QEMU restart: install it on every boot"
            if mode == "init":
                assert persisted_sequence == 0
                return (0, zero_hash)
            assert mode == "verify", "unexpected worker operation"
            return (persisted_sequence, f"{persisted_sequence:064x}")

        def fake_cut(_args, _port, _qemu, acknowledged, round_index, _case):
            nonlocal persisted_sequence, installed
            assert installed, "tried to write using missing worker"
            assert acknowledged == persisted_sequence
            accepted = acknowledged + 1
            # One extra, unacknowledged commit survives the first power cut;
            # none survives the second. Both outcomes are supported.
            persisted_sequence = accepted + (1 if round_index == 0 else 0)
            installed = False
            return accepted, f"{accepted:064x}"

        with patch.object(runner, "execute", side_effect=fake_execute), \
             patch.object(runner, "unused_port", return_value=22022), \
             patch.object(runner, "start_vm", side_effect=fake_start), \
             patch.object(runner, "stop_vm"), \
             patch.object(runner, "await_ssh"), \
             patch.object(runner, "setup_mount", side_effect=fake_mount), \
             patch.object(runner, "install_worker", side_effect=fake_install), \
             patch.object(runner, "worker_state", side_effect=fake_state), \
             patch.object(runner, "powercut_round", side_effect=fake_cut):
            result = runner.run_fs(args, root, "ext4")

        assert boots == 3 and install_count == 3, (boots, install_count)
        assert mounted == [True, False, False], mounted
        assert result["result"] == "passed"
        assert result["final_sequence"] == 3
        assert len(result["rounds"]) == 2
        assert result["rounds"][0]["acknowledged"] == 1
        assert result["rounds"][0]["recovered_sequence"] == 2
        assert result["rounds"][1]["acknowledged"] == 3
        assert result["rounds"][1]["recovered_sequence"] == 3


def check_deletion_reboot_runner(runner):
    expected = [("branch", "before-intent"), ("truncate", "before-intent"),
                ("branch", "after-intent"), ("truncate", "after-intent"),
                ("branch", "after-ref"), ("truncate", "after-ref"),
                ("branch", "after-drafts"), ("truncate", "after-drafts"),
                ("branch", "after-finish"), ("truncate", "after-finish")]
    assert [runner.deletion_case(index) for index in range(10)] == expected
    with tempfile.TemporaryDirectory(prefix="aowis-vm-deletion-runner-") as location:
        root = Path(location)
        image = root / "guest.qcow2"
        image.write_bytes(b"mock image")
        args = SimpleNamespace(guest_image=image, disk_mib=256, rounds=3,
                               cloud_init_seed=None)
        boots = []
        verified = []
        cuts = []
        installed = []

        def fake_execute(command, **_kwargs):
            if command[:3] == ["qemu-img", "info", "--output=json"]:
                return json.dumps({"format": "qcow2"})
            if command[:3] == ["qemu-img", "create", "-f"]:
                return ""
            raise AssertionError("unexpected command: " + repr(command))

        def fake_cut(_args, _port, _qemu, number, _case):
            scenario, point = runner.deletion_case(number)
            assert len(installed) == number + 1
            cuts.append((scenario, point))
            return {"round": number, "scenario": scenario, "barrier": point,
                    "expected_recovery": "original" if point == "before-intent" else "deleted"}

        def fake_state(_args, _port, number, expected_recovery):
            assert len(installed) > number + 1, "verification occurred before reboot"
            assert expected_recovery == ("original" if number < 2 else "deleted")
            verified.append((number, expected_recovery))

        with patch.object(runner, "execute", side_effect=fake_execute), \
             patch.object(runner, "unused_port", return_value=22022), \
             patch.object(runner, "start_vm", side_effect=lambda *_: boots.append(1) or object()), \
             patch.object(runner, "stop_vm"), \
             patch.object(runner, "await_ssh"), \
             patch.object(runner, "setup_mount"), \
             patch.object(runner, "install_worker", side_effect=lambda *_: installed.append(1)), \
             patch.object(runner, "deletion_powercut_round", side_effect=fake_cut), \
             patch.object(runner, "deletion_state", side_effect=fake_state):
            result = runner.run_fs_deletions(args, root, "xfs")
        assert len(boots) == len(installed) == 4
        assert cuts == expected[:3]
        assert verified == [(0, "original"), (0, "original"),
                            (1, "original"), (0, "original"),
                            (1, "original"), (2, "deleted")], verified
        assert len(result["rounds"]) == 3 and result["result"] == "passed"
        assert all(entry["verified_after_reboot"] for entry in result["rounds"])


def check_continuous_deletion_reboots(runner):
    with tempfile.TemporaryDirectory(prefix="aowis-vm-stress-runner-") as location:
        root = Path(location)
        image = root / "guest.qcow2"
        image.write_bytes(b"mock image")
        args = SimpleNamespace(guest_image=image, disk_mib=256, rounds=2,
                               cloud_init_seed=None)
        remote_calls = []
        boots = []
        cuts = []

        def fake_execute(command, **_kwargs):
            if command[:3] == ["qemu-img", "info", "--output=json"]:
                return json.dumps({"format": "qcow2"})
            if command[:3] == ["qemu-img", "create", "-f"]:
                return ""
            raise AssertionError("unexpected command: " + repr(command))

        def fake_remote(_args, _port, worker, mode, path, **_kwargs):
            assert worker == runner.WORKER and path == runner.MOUNT + "/stress"
            remote_calls.append((mode, len(boots)))
            return "STRESS-VERIFIED 0" if mode == "stress-verify" else "STRESS-READY"

        def fake_cut(_args, _port, _qemu, number, _case):
            cuts.append(number)
            return {"round": number}

        with patch.object(runner, "execute", side_effect=fake_execute), \
             patch.object(runner, "unused_port", return_value=22022), \
             patch.object(runner, "start_vm", side_effect=lambda *_: boots.append(1) or object()), \
             patch.object(runner, "stop_vm"), \
             patch.object(runner, "await_ssh"), \
             patch.object(runner, "setup_mount"), \
             patch.object(runner, "install_worker"), \
             patch.object(runner, "remote", side_effect=fake_remote), \
             patch.object(runner, "stress_powercut_round", side_effect=fake_cut):
            result = runner.run_fs_deletion_stress(args, root, "ext4")
        assert len(boots) == 3 and cuts == [0, 1]
        assert remote_calls == [("stress-init", 1), ("stress-verify", 2),
                                ("stress-verify", 3)]
        assert result["result"] == "passed"
        assert all(entry["verified_after_reboot"] for entry in result["rounds"])


def main():
    runner = load_runner(Path(__file__).resolve().parents[1] / "tools" / "qualify_vm_powercut.py")
    check_worker_installed_after_each_boot(runner)
    check_deletion_reboot_runner(runner)
    check_continuous_deletion_reboots(runner)
    print("QEMU orchestration smoke passed: commits and deletion reboot verification")


if __name__ == "__main__":
    main()
