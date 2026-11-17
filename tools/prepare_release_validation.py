#!/usr/bin/env python3
"""Keep release builds immutable while validating committed host-test repairs."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def git_command(root: Path, *args: str) -> list[str]:
    return ["git", "-c", f"safe.directory={root}", "-C", str(root), *args]


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(git_command(root, *args), text=True).strip()


def unchanged(root: Path) -> None:
    if git(root, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Tracked release/validation sources must be clean")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(root: Path, validation: str, output: Path) -> dict:
    unchanged(root)
    source = git(root, "rev-parse", "HEAD")
    target = source
    changed: list[str] = []
    validation_root = root
    if validation:
        if not re.fullmatch(r"[0-9a-f]{40}", validation):
            raise ValueError("Validation commit must be an exact lowercase 40-digit SHA")
        available = subprocess.run(
            git_command(root, "cat-file", "-e", validation + "^{commit}"),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if available.returncode:
            subprocess.run(git_command(root, "fetch", "--no-tags",
                                       "--depth=1", "origin", validation), check=True)
        target = git(root, "rev-parse", validation + "^{commit}")
        changes = subprocess.check_output(
            git_command(root, "diff", "--no-renames", "--name-only",
                        "-z", source, target)).decode().split("\0")
        changed = [path for path in changes if path]
        if any(not path.startswith("tools/") for path in changed):
            raise ValueError("Validation commit changes files outside tools/")
        if target != source:
            validation_root = Path(tempfile.mkdtemp(
                prefix="release-host-validation-", dir=os.environ.get("RUNNER_TEMP")))
            subprocess.run(git_command(root, "worktree", "add", "--detach",
                                       str(validation_root), target), check=True)
            for name in ("build", "managed_components", "sdkconfig"):
                original = root / name
                if original.exists():
                    (validation_root / name).symlink_to(
                        original, target_is_directory=original.is_dir())
            unchanged(validation_root)
    output.parent.mkdir(parents=True, exist_ok=True)
    patch = output.with_name("host-validation-patch.diff")
    patch.write_bytes(subprocess.check_output(
        git_command(root, "diff", "--binary", source, target, "--", "tools/")))
    result = {
        "schema": 1, "sourceCommit": source, "validationCommit": target,
        "workflowCommit": os.environ.get("GITHUB_SHA", source),
        "sourceRoot": str(root), "validationRoot": str(validation_root),
        "changedFiles": changed, "runtimeFilesMatch": True,
        "appSha256BeforeValidation": digest(root / "build/Read_Pico.bin"),
        "patchSha256": digest(patch), "verifiedAfterTests": False,
    }
    output.write_text(json.dumps(result, indent=2) + "\n")
    if os.environ.get("GITHUB_ENV"):
        with Path(os.environ["GITHUB_ENV"]).open("a") as environment:
            environment.write(f"HOST_VALIDATION_ROOT={validation_root}\n")
    return result


def verify(root: Path, output: Path) -> dict:
    result = json.loads(output.read_text())
    unchanged(root)
    validation_root = Path(result["validationRoot"])
    unchanged(validation_root)
    if str(root) != result["sourceRoot"] or git(root, "rev-parse", "HEAD") != result["sourceCommit"]:
        raise ValueError("Release source identity changed during validation")
    if git(validation_root, "rev-parse", "HEAD") != result["validationCommit"]:
        raise ValueError("Host validation source identity changed")
    if digest(root / "build/Read_Pico.bin") != result["appSha256BeforeValidation"]:
        raise ValueError("Official application changed during host validation")
    if digest(output.with_name("host-validation-patch.diff")) != result["patchSha256"]:
        raise ValueError("Host validation patch changed")
    result["verifiedAfterTests"] = True
    output.write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--validation-commit", default="")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root = args.source.resolve()
    output = args.output or root / "build/validation-provenance.json"
    result = verify(root, output) if args.verify else prepare(root, args.validation_commit, output)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
