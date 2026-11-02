#!/usr/bin/env python3
"""Run the MODEL build-time content fingerprint generator without Qt."""
import argparse
import pathlib
import re
import subprocess
import tempfile
import time


def generate(cmake, model, target):
    project = target.parent / ("harness-" + target.name)
    project.mkdir(exist_ok=True)
    (project / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.22)\nproject(FingerprintTest LANGUAGES NONE)\n'
        'include("%s")\naowis_generate_model_codec_fingerprint("%s" "%s")\n' %
        (cmake.as_posix(), model.as_posix(), target.as_posix()))
    subprocess.run(["cmake", "-S", str(project), "-B", str(target)],
                   check=True, capture_output=True)
    content = (target / "aowis_model_codec_fingerprint.h").read_text()
    match = re.search(r'AOWIS_MODEL_CODEC_FINGERPRINT "([0-9a-f]{64})"', content)
    if not match:
        raise AssertionError("generated fingerprint is not a SHA-256 digest")
    return match.group(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=pathlib.Path, required=True)
    parser.add_argument("--model", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        base = pathlib.Path(tmp)
        sources = base / "model"
        (sources / "src").mkdir(parents=True)
        (sources / "include").mkdir()
        (sources / "src" / "codec.cpp").write_text("version A\n")
        (sources / "include" / "codec.h").write_text("header A\n")
        first = generate(args.cmake.resolve(), sources, base / "build1")
        second = generate(args.cmake.resolve(), sources, base / "build2")
        assert first == second, "identical sources gave different fingerprints"
        # Some generators/filesystems compare timestamps at one-second resolution.
        time.sleep(1.1)
        (sources / "src" / "codec.cpp").write_text("version B\n")
        # Do NOT call CMake configure here: a normal incremental build must
        # notice modified MODEL bytes and regenerate the embedded fingerprint.
        subprocess.run(["cmake", "--build", str(base / "build1")],
                       check=True, capture_output=True)
        header = (base / "build1" / "aowis_model_codec_fingerprint.h").read_text()
        changed_match = re.search(r'AOWIS_MODEL_CODEC_FINGERPRINT "([0-9a-f]{64})"', header)
        assert changed_match is not None
        changed = changed_match.group(1)
        assert changed != first, "incremental build ignored an uncommitted MODEL edit"
        (sources / "src" / "codec.cpp").write_text("version A\n")
        restored = generate(args.cmake.resolve(), sources, base / "build1")
        assert restored == first, "restoring source bytes did not restore fingerprint"
        (sources / "include" / "new_field.h").write_text("new field\n")
        assert generate(args.cmake.resolve(), sources, base / "build1") != first, \
            "new header was not reflected in fingerprint"
    print("MODEL content fingerprint reproducibility and change detection passed")


if __name__ == "__main__":
    main()
