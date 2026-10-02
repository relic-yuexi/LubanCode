"""Relocated SDK header gate fixtures; no configure, build or native execution."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "check_installed_sdk.py"
SPEC = importlib.util.spec_from_file_location("sdk_installed", SCRIPT)
installed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installed)

FOCUSED_SPEC = importlib.util.spec_from_file_location("sdk_focused", SCRIPT.with_name("check_sdk_focused.py"))
focused = importlib.util.module_from_spec(FOCUSED_SPEC)
FOCUSED_SPEC.loader.exec_module(focused)


class MemoryCasWindowsPathsTests(unittest.TestCase):
    markers = ("[memory-cas-path] target-extended", "[memory-cas-path] temporary-threshold")

    def test_windows_requires_both_unique_native_markers_and_posix_requires_neither(self):
        focused.check_memory_cas_paths("\n".join(self.markers) + "\n", "nt")
        focused.check_memory_cas_paths("", "posix")

    def test_windows_rejects_missing_or_decorated_native_markers(self):
        for absent in self.markers:
            with self.subTest(absent=absent):
                body = "\n".join(marker for marker in self.markers if marker != absent)
                with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                    focused.check_memory_cas_paths(body, "nt")
                with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                    focused.check_memory_cas_paths(body + "\nother-source: " + absent, "nt")
        with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
            focused.check_memory_cas_paths("", "nt")

    def test_windows_rejects_each_duplicated_native_marker(self):
        body = "\n".join(self.markers) + "\n"
        for duplicate in self.markers:
            with self.subTest(duplicate=duplicate):
                with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                    focused.check_memory_cas_paths(body + duplicate + "\n", "nt")


class InstalledHeadersTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-extension-gate-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name)
        self.headers = {
            "include/lubancore/api.hpp", "include/lubancore/core.hpp",
            "include/lubancore/extensions.hpp", "include/lubancore/detail/types.hpp",
            "include/lubancore/results.hpp",
            "include/lubancore/skills.hpp",
            "include/lubancore/memory.hpp",
            "include/lubancore/subagents.hpp",
        }
        for relative in self.headers:
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n", encoding="utf-8")

    def test_component_and_full_install_accept_all_recursive_public_headers(self):
        for mode in ("component", "full"):
            with self.subTest(mode=mode):
                self.assertEqual(installed.check_public_headers(self.repo, sorted(self.headers), mode),
                                 self.headers)

    def test_both_install_modes_reject_each_missing_public_header(self):
        for mode in ("component", "full"):
            for missing in sorted(self.headers):
                with self.subTest(mode=mode, missing=missing):
                    with self.assertRaisesRegex(RuntimeError, f"{mode} SDK install is missing public headers") as error:
                        installed.check_public_headers(self.repo, sorted(self.headers - {missing}), mode)
                    self.assertIn(missing, str(error.exception))

    def test_missing_extension_or_result_source_cannot_shrink_the_required_install_contract(self):
        for missing in ("include/lubancore/extensions.hpp", "include/lubancore/results.hpp", "include/lubancore/skills.hpp", "include/lubancore/memory.hpp"):
            path = self.repo / missing
            contents = path.read_text(encoding="utf-8")
            path.unlink()
            try:
                for mode in ("component", "full"):
                    with self.subTest(mode=mode, missing=missing):
                        with self.assertRaisesRegex(RuntimeError, "SDK source is missing required public headers") as error:
                            installed.check_public_headers(self.repo, sorted(self.headers - {missing}), mode)
                        self.assertIn(missing, str(error.exception))
            finally:
                path.write_text(contents, encoding="utf-8")


class InstalledSearchResourcesTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-search-resource-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name) / "repo"
        self.prefix = Path(self.scratch.name) / "relocated"
        self.stage = Path(self.scratch.name) / "stage"
        self.stage.mkdir()
        for binary in ("rg", "rg.exe"):
            (self.stage / binary).write_bytes(b"data fixture, never executed")
        self.pairs = {
            "share/lubancore/libexec/rg": self.stage / "rg",
            "share/lubancore/libexec/rg.exe": self.stage / "rg.exe",
            "share/lubancore/licenses/ripgrep/LICENSE-MIT": self.repo / "third_party/ripgrep/LICENSE-MIT",
            "share/lubancore/ripgrep-manifest.json": self.repo / "third_party/ripgrep/manifest.json",
        }
        for relative, original in self.pairs.items():
            original.parent.mkdir(parents=True, exist_ok=True)
            if not original.exists():
                original.write_bytes(b"nonempty repository resource fixture")
            installed_file = self.prefix / relative
            installed_file.parent.mkdir(parents=True, exist_ok=True)
            installed_file.write_bytes(original.read_bytes())
            installed_file.chmod(0o755)

    def test_relocated_platform_resources_preserve_input_bytes(self):
        for platform, binary in (("win32", "rg.exe"), ("linux", "rg"), ("darwin", "rg")):
            with self.subTest(platform=platform):
                facts = installed.check_search_resources(self.repo, self.prefix, self.stage, platform)
                self.assertEqual(len(facts), 3)
                self.assertIn("share/lubancore/libexec/" + binary, facts)

    def test_missing_or_changed_backend_license_or_manifest_fails(self):
        for relative in self.pairs:
            platform = "win32" if relative.endswith(".exe") else "linux"
            path = self.prefix / relative
            contents = path.read_bytes()
            with self.subTest(resource=relative):
                path.unlink()
                with self.assertRaisesRegex(RuntimeError, "resource is missing"):
                    installed.check_search_resources(self.repo, self.prefix, self.stage, platform)
                path.write_bytes(contents + b"changed")
                path.chmod(0o755)
                with self.assertRaisesRegex(RuntimeError, "differs from"):
                    installed.check_search_resources(self.repo, self.prefix, self.stage, platform)
                path.write_bytes(contents)
                path.chmod(0o755)

    def test_posix_relocation_cannot_lose_executable_permission(self):
        # Pure permission-result injection also exercises this negative gate on
        # Windows; real POSIX installed bytes/mode are checked by remote CI.
        with patch.object(installed.os, "access", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "lost executable permission"):
                installed.check_search_resources(self.repo, self.prefix, self.stage, "linux")


if __name__ == "__main__":
    unittest.main()
