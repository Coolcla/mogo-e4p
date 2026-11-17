#!/usr/bin/env python3
"""Offline Git fixtures for immutable release builds and repaired host harnesses."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("prepare_release_validation.py")


class ReleaseValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "repo"
        self.root.mkdir()
        self.env = {**os.environ, "RUNNER_TEMP": str(self.base),
                    "GITHUB_ENV": str(self.base / "github-env")}
        self.git("init", "-q")
        self.git("config", "user.name", "Release Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.root / ".gitignore").write_text("build/\nmanaged_components/\nsdkconfig\n")
        (self.root / "product.c").write_text("original product\n")
        (self.root / "tools").mkdir()
        (self.root / "tools/test.py").write_text("original test\n")
        self.source = self.commit()
        (self.root / "build").mkdir()
        (self.root / "build/Read_Pico.bin").write_bytes(b"official build bytes")
        (self.root / "managed_components").mkdir()
        (self.root / "sdkconfig").write_text("official config\n")

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.root), *args], text=True, stderr=subprocess.DEVNULL).strip()

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def run_helper(self, *args, expected=0):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--source", str(self.root), *args],
            env=self.env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        output = self.root / "build/validation-provenance.json"
        return json.loads(output.read_text()) if result.returncode == 0 else result

    def repaired_commit(self):
        (self.root / "tools/test.py").write_text("repaired test\n")
        target = self.commit()
        self.git("checkout", "--detach", "-q", self.source)
        return target

    def test_default_keeps_source_and_app(self):
        result = self.run_helper()
        self.assertEqual(result["sourceCommit"], self.source)
        self.assertEqual(result["validationCommit"], self.source)
        self.assertEqual(result["changedFiles"], [])
        self.assertTrue(self.run_helper("--verify")["verifiedAfterTests"])

    def test_committed_test_repairs_use_independent_checkout(self):
        target = self.repaired_commit()
        result = self.run_helper("--validation-commit", target)
        validation = Path(result["validationRoot"])
        self.assertEqual((self.root / "tools/test.py").read_text(), "original test\n")
        self.assertEqual((validation / "tools/test.py").read_text(), "repaired test\n")
        self.assertEqual((validation / "product.c").read_bytes(), (self.root / "product.c").read_bytes())
        self.assertTrue((validation / "build").is_symlink())
        self.assertEqual(result["validationCommit"], target)
        self.assertEqual(result["changedFiles"], ["tools/test.py"])
        self.assertEqual(result["appSha256BeforeValidation"],
                         hashlib.sha256(b"official build bytes").hexdigest())
        self.assertTrue(self.run_helper("--verify")["verifiedAfterTests"])

    def test_product_changes_are_rejected(self):
        (self.root / "product.c").write_text("changed product\n")
        target = self.commit()
        self.git("checkout", "--detach", "-q", self.source)
        result = self.run_helper("--validation-commit", target, expected=1)
        self.assertIn("outside tools/", result.stderr)

    def test_symbolic_and_malformed_refs_are_rejected(self):
        for reference in ("main", "abc123", "../branch", "A" * 40):
            result = self.run_helper("--validation-commit", reference, expected=1)
            self.assertIn("exact lowercase", result.stderr)

    def test_dirty_sources_are_rejected(self):
        (self.root / "product.c").write_text("dirty product\n")
        result = self.run_helper(expected=1)
        self.assertIn("must be clean", result.stderr)

    def test_changed_application_is_rejected_after_tests(self):
        self.run_helper()
        (self.root / "build/Read_Pico.bin").write_bytes(b"changed image")
        result = self.run_helper("--verify", expected=1)
        self.assertIn("application changed", result.stderr)

    def test_changed_validation_source_is_rejected(self):
        result = self.run_helper("--validation-commit", self.repaired_commit())
        (Path(result["validationRoot"]) / "tools/test.py").write_text("uncommitted test\n")
        self.assertIn("must be clean", self.run_helper("--verify", expected=1).stderr)


if __name__ == "__main__":
    unittest.main()
