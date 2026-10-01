"""Relocated SDK header gate fixtures; no configure, build or native execution."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "check_installed_sdk.py"
SPEC = importlib.util.spec_from_file_location("sdk_installed", SCRIPT)
installed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installed)


class InstalledHeadersTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-extension-gate-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name)
        self.headers = {
            "include/lubancore/api.hpp", "include/lubancore/core.hpp",
            "include/lubancore/extensions.hpp", "include/lubancore/detail/types.hpp",
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

    def test_missing_extension_source_cannot_shrink_the_required_install_contract(self):
        missing = "include/lubancore/extensions.hpp"
        (self.repo / missing).unlink()
        for mode in ("component", "full"):
            with self.subTest(mode=mode):
                with self.assertRaisesRegex(RuntimeError, "SDK source is missing required public headers") as error:
                    installed.check_public_headers(self.repo, sorted(self.headers - {missing}), mode)
                self.assertIn(missing, str(error.exception))


if __name__ == "__main__":
    unittest.main()
