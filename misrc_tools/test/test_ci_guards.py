#!/usr/bin/env python3
"""Regression tests for the required RTL-SDR release-build guards."""

import contextlib
import io
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ci_guard_tests as guards


class RtlSdrGuardTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        workflow = Path(__file__).resolve().parents[2] / ".github/workflows/build.yml"
        self.workflow = workflow.read_text(encoding="utf-8")

    def check_workflow(self, text):
        path = self.path / "build.yml"
        path.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
        with contextlib.redirect_stderr(io.StringIO()):
            return guards.check_workflow_rtlsdr_policy(path)

    def replace_in_job(self, job, old, new, occurrence=0):
        match = re.search(rf"^  {re.escape(job)}:.*?(?=^  [\w-]+:|\Z)",
                          self.workflow, re.M | re.S)
        self.assertIsNotNone(match, job)
        body = match.group()
        positions = [m.start() for m in re.finditer(re.escape(old), body)]
        self.assertGreater(len(positions), occurrence, old)
        start = positions[occurrence]
        changed = body[:start] + new + body[start + len(old):]
        return self.workflow[:match.start()] + changed + self.workflow[match.end():]

    def test_current_workflow_passes(self):
        self.assertEqual(self.check_workflow(self.workflow), 0)

    def test_each_windows_install_list_requires_its_libraries(self):
        for job, prefix in (("windows-exe", "mingw-w64-x86_64-"),
                            ("windows-exe-arm64", "mingw-w64-clang-aarch64-")):
            libraries = ("rtl-sdr", "libsoxr") if job == "windows-exe" else ("rtl-sdr",)
            for library in libraries:
                for occurrence in (0, 1):
                    with self.subTest(job=job, library=library, occurrence=occurrence):
                        changed = self.replace_in_job(job, prefix + library, "", occurrence)
                        self.assertNotEqual(self.check_workflow(changed), 0)

    def test_each_build_job_requires_probe_and_binary_guard(self):
        for job in ("windows-exe", "windows-exe-arm64", "macos-app-build"):
            probe = "pkg-config --modversion librtlsdr"
            if job != "windows-exe-arm64":
                probe += " soxr"
            for snippet in (probe, "--require-rtlsdr"):
                with self.subTest(job=job, snippet=snippet):
                    changed = self.replace_in_job(job, snippet, "")
                    self.assertNotEqual(self.check_workflow(changed), 0)

    def test_missing_required_job_fails(self):
        for job in ("windows-exe", "windows-exe-arm64", "macos-app-build",
                    "macos-app-universal"):
            with self.subTest(job=job):
                changed = self.workflow.replace(f"  {job}:\n", f"  removed-{job}:\n", 1)
                self.assertNotEqual(self.check_workflow(changed), 0)

    def test_macos_dependency_and_packaging_are_required(self):
        for job, snippet in (
            ("macos-app-build", "libsoxr librtlsdr; do"),
            ("macos-app-build", 'test -n "$RTLSDR_DYLIB"'),
            ("macos-app-build", 'test -f "$FW_DIR/${RTLSDR_DYLIB#@rpath/}"'),
            ("macos-app-universal", 'lipo -verify_arch arm64 x86_64 "$DYLIB"'),
        ):
            with self.subTest(snippet=snippet):
                changed = self.replace_in_job(job, snippet, "")
                self.assertNotEqual(self.check_workflow(changed), 0)

    def test_binary_requires_both_backend_markers(self):
        opened = b"[RTL-SDR] Opened device "
        capture = b"[RTL-SDR] Starting capture"
        for content, expected in ((opened + b"\x00" + capture, 0),
                                  (b"RTL-SDR", 1), (opened, 1), (capture, 1), (b"", 1)):
            with self.subTest(content=content):
                binary = self.path / "misrc_gui.exe"
                binary.write_bytes(content)
                with contextlib.redirect_stderr(io.StringIO()):
                    result = guards.check_built_gui_has_rtlsdr_backend(binary)
                self.assertEqual(result, expected)

    def test_missing_or_unreadable_binary_fails(self):
        for path in (self.path / "missing.exe", self.path):
            with self.subTest(path=path), contextlib.redirect_stderr(io.StringIO()):
                self.assertNotEqual(guards.check_built_gui_has_rtlsdr_backend(path), 0)

    def test_required_backend_needs_post_build_and_existing_binary(self):
        for args in (("--require-rtlsdr",),
                     ("--require-rtlsdr", "--post-build"),
                     ("--require-rtlsdr", "--post-build", "--gui-path",
                      str(self.path / "missing.exe"))):
            with self.subTest(args=args), mock.patch.object(sys, "argv", ["ci_guard_tests.py", *args]), \
                    contextlib.redirect_stderr(io.StringIO()):
                try:
                    result = guards.main()
                except SystemExit as error:
                    result = error.code
                self.assertNotEqual(result, 0)


if __name__ == "__main__":
    unittest.main()
