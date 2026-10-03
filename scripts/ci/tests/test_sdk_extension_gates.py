"""Relocated SDK header gate fixtures; no configure, build or native execution."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
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


class PackageNativeEvidenceTests(unittest.TestCase):
    def evidence(self, count, executable="lubancore_sdk_tests"):
        source = "test_package_manifest.cpp" if count == 15 else "test_lubancore_package_manifest.cpp"
        command = ["C:/actual build/" + executable + ".exe", "--source-file=*" + source]
        body = (f'Command: "{command[0]}" "{command[1]}"\n'
                f'[doctest] test cases: {count} | {count} passed | 0 failed\n'
                '[doctest] assertions: 20 | 20 passed | 0 failed')
        return command, body

    def test_original_and_public_source_filters_cannot_be_replaced_or_reduced(self):
        for source in ("test_package_manifest.cpp", "test_lubancore_package_manifest.cpp"):
            for executable in ("lubancode_tests", "lubancore_sdk_tests"):
                command = ["C:/actual build/" + executable + ".exe", "--source-file=*" + source]
                focused.check_package_registration(command, source, executable)
                for wrong in ([], ["fake", command[1]], [command[0], "--source-file=*other.cpp"],
                              [*command, "--test-case=only-one"]):
                    with self.subTest(source=source, command=wrong), self.assertRaises(RuntimeError):
                        focused.check_package_registration(wrong, source, executable)

    def test_exact_original_and_public_native_case_rosters(self):
        for count in (15, 8):
            for executable in ("lubancode_tests", "lubancore_sdk_tests"):
                command, body = self.evidence(count, executable)
                focused.check_package_native(body, count, command)
                focused.check_package_native(body.replace("\n", "\r\n"), count, command)

    def test_same_basename_foreign_checkout_path_and_extra_actual_filters_reject(self):
        for count in (15, 8):
            command, body = self.evidence(count)
            for bad in (body.replace("C:/actual build/", "D:/foreign checkout/"),
                        body.replace('"--source-file=*', '"--source-file=*other-'),
                        body.replace('"\n[doctest]', '" "--test-case=one"\n[doctest]'),
                        body + '\nCommand: "fake"', body.replace('Command:', 'Unrelated:')):
                with self.subTest(count=count, bad=bad), self.assertRaises(RuntimeError):
                    focused.check_package_native(bad, count, command)

    def test_registration_failure_retains_original_stdout_stderr_and_exitcode(self):
        with tempfile.TemporaryDirectory() as scratch:
            failed = subprocess.CompletedProcess([], 17, b"\xffraw-stdout\n", b"\xffraw-stderr\n")
            with patch("sys.argv", ["check", "--build-dir", scratch]), \
                    patch.object(focused.subprocess, "run", return_value=failed) as mocked:
                with self.assertRaises(subprocess.CalledProcessError):
                    focused.main()
                self.assertEqual(mocked.call_count, 1)
                self.assertNotIn("check", mocked.call_args.kwargs)
            evidence = Path(scratch) / "test-evidence/sdk-focused"
            self.assertEqual((evidence / "registration.stdout").read_bytes(), failed.stdout)
            self.assertEqual((evidence / "tests.json").read_bytes(), failed.stdout)
            self.assertEqual((evidence / "registration.stderr").read_bytes(), failed.stderr)
            result = json.loads((evidence / "registration-result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["returncode"], 17)
            self.assertEqual(result["command"][-1], "--show-only=json-v1")

    def test_missing_duplicate_changed_skipped_and_empty_native_reports_reject(self):
        for count in (15, 8):
            command, body = self.evidence(count)
            for invalid in ('', body + '\n' + body, body.replace(f'{count} passed', '0 passed'),
                            body.replace('20 | 20 passed', '0 | 0 passed'),
                            body.replace('20 passed | 0 failed', '19 passed | 1 failed'),
                            body.replace(f'test cases: {count}', f'test cases: {count + 1}')):
                with self.subTest(count=count, invalid=invalid), self.assertRaises(RuntimeError):
                    focused.check_package_native(invalid, count, command)


class ResultStoreWindowsPathsTests(unittest.TestCase):
    summary = "[doctest] test cases: 17 | 17 passed | 0 failed"
    markers = ("[result-store-path] target-extended", "[result-store-path] temporary-threshold",
               "[result-store-path-length] target-extended target=340 temporary=344",
               "[result-store-path-length] temporary-threshold target=247 temporary=251")

    def test_actual_windows_pair_and_original_posix_roster(self):
        focused.check_result_store_native("\n".join((self.summary, *self.markers)), "nt")
        focused.check_result_store_native(self.summary, "posix")

    def test_missing_decorated_and_duplicate_windows_markers_reject(self):
        for marker in self.markers:
            with self.subTest(marker=marker):
                body = "\n".join((self.summary, *(value for value in self.markers if value != marker)))
                for changed in (body, body + "\nother-source: " + marker,
                                "\n".join((self.summary, *self.markers, marker))):
                    with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                        focused.check_result_store_native(changed, "nt")

    def test_wrong_real_path_length_rejects(self):
        body = "\n".join((self.summary, *self.markers)).replace("temporary=251", "temporary=247")
        with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
            focused.check_result_store_native(body, "nt")

    def test_empty_or_changed_native_roster_rejects(self):
        for body in ("", "[doctest] test cases: 0 | 0 passed | 0 failed",
                     self.summary.replace("17 passed | 0 failed", "16 passed | 1 failed"),
                     self.summary + "\n" + self.summary):
            with self.subTest(body=body), self.assertRaisesRegex(RuntimeError, "17 successful cases"):
                focused.check_result_store_native(body, "posix")


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


class PlanRetryEvidenceTests(unittest.TestCase):
    def body(self, platform="posix", executable="lubancore_sdk_tests"):
        count = 25 if platform == "nt" else 22
        paths = focused.PLAN_RETRY_PATHS
        if platform == "nt":
            paths = (*paths, "windows-sharing-recovery")
        return "\n".join((
            f'Command: "C:/actual build/{executable}.exe" "--source-file=*test_atomic_write.cpp"',
            f"[doctest] test cases: {count} | {count} passed | 0 failed | 200 skipped",
            "[doctest] assertions: 120 | 120 passed | 0 failed |",
            *("[sdk-plan-retry] " + path for path in paths), "Test Passed."))

    def test_exact_windows_and_posix_native_and_asan_command(self):
        focused.check_plan_retry_native(self.body("nt"), "nt")
        focused.check_plan_retry_native(self.body(), "posix")
        focused.check_plan_retry_native(self.body(executable="lubancode_tests"), "posix", "lubancode_tests")

    def test_empty_old_wrong_and_failed_native_rosters_reject(self):
        for platform, old in (("nt", 19), ("posix", 16)):
            count = 25 if platform == "nt" else 22
            for bad in (0, old, count - 1, count + 1):
                body = self.body(platform).replace(f"{count} | {count} passed", f"{bad} | {bad} passed")
                with self.subTest(platform=platform, bad=bad), self.assertRaisesRegex(RuntimeError, "native roster"):
                    focused.check_plan_retry_native(body, platform)
            for body in (self.body(platform).replace(f"{count} passed | 0 failed", f"{count - 1} passed | 1 failed"),
                         self.body(platform) + f"\n[doctest] test cases: {count} | {count} passed | 0 failed"):
                with self.assertRaisesRegex(RuntimeError, "native roster"):
                    focused.check_plan_retry_native(body, platform)

    def test_missing_decorated_or_duplicate_path_cannot_borrow_success(self):
        for path in (*focused.PLAN_RETRY_PATHS, "windows-sharing-recovery"):
            marker = "[sdk-plan-retry] " + path
            for body in (self.body("nt").replace(marker, ""),
                         self.body("nt").replace(marker, "other-source: " + marker),
                         self.body("nt") + "\n" + marker):
                with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, "actual path did not finish once"):
                    focused.check_plan_retry_native(body, "nt")

    def test_wrong_source_wrong_binary_and_extra_filter_reject(self):
        for command in ([], ["lubancore_sdk_tests", "--source-file=*test_other.cpp"],
                        ["other_tests", "--source-file=*test_atomic_write.cpp"],
                        ["lubancore_sdk_tests", "--source-file=*test_atomic_write.cpp", "--test-case=one"]):
            with self.subTest(command=command), self.assertRaisesRegex(RuntimeError, "single actual atomic-write source"):
                focused.check_plan_retry_registration(command)
        with self.assertRaisesRegex(RuntimeError, "single actual atomic-write source"):
            focused.check_plan_retry_native(self.body().replace("test_atomic_write.cpp", "test_other.cpp"), "posix")
        with self.assertRaisesRegex(RuntimeError, "identify one actual command"):
            focused.check_plan_retry_native(self.body() + '\nCommand: "lubancore_sdk_tests"', "posix")

    def test_empty_failed_assertions_or_missing_ctest_pass_reject(self):
        for body in (self.body().replace("120 | 120 passed", "0 | 0 passed"),
                     self.body().replace("120 passed | 0 failed", "119 passed | 1 failed"),
                     self.body().replace("Test Passed.", "Test Failed."),
                     self.body() + "\n[doctest] assertions: 120 | 120 passed | 0 failed |"):
            with self.assertRaisesRegex(RuntimeError, "assertions did not actually pass"):
                focused.check_plan_retry_native(body, "posix")

    def test_posix_cannot_claim_windows_native_handle(self):
        with self.assertRaisesRegex(RuntimeError, "cannot claim a Windows sharing probe"):
            focused.check_plan_retry_native(self.body() + "\n[sdk-plan-retry] windows-sharing-recovery", "posix")


class InstalledHeadersTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-extension-gate-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name)
        self.headers = {
            "include/lubancore/packages.hpp",
            "include/lubancore/api.hpp", "include/lubancore/core.hpp",
            "include/lubancore/extensions.hpp", "include/lubancore/detail/types.hpp",
            "include/lubancore/results.hpp",
            "include/lubancore/skills.hpp",
            "include/lubancore/memory.hpp",
            "include/lubancore/subagents.hpp",
            "include/lubancore/lua.hpp",
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


class LuaEvidenceTests(unittest.TestCase):
    def body(self, protected=False, executable="lubancore_sdk_tests"):
        source = "test_lua_protected.cpp" if protected else "test_lubancore_lua.cpp"
        count = 6 if protected else 9
        return "\n".join((
            f'Command: "C:/actual build/{executable}.exe" "--source-file=*{source}"',
            f"[doctest] test cases: {count} | {count} passed | 0 failed | 100 skipped",
            "[doctest] assertions: 32 | 32 passed | 0 failed |",
            *("[sdk-lua-path] " + path for path in focused.LUA_PATHS if not protected),
            "Test Passed.",
        ))

    def test_actual_source_counts_and_paths(self):
        focused.check_lua_native(self.body())
        focused.check_lua_native(self.body(True), protected=True)
        focused.check_lua_native(self.body(executable="lubancode_tests"), executable="lubancode_tests")

    def test_empty_failed_wrong_source_or_borrowed_binary(self):
        body = self.body()
        for changed in (body.replace("9 | 9 passed", "0 | 0 passed"),
                        body.replace("9 | 9 passed | 0 failed", "9 | 8 passed | 1 failed"),
                        body.replace("32 | 32 passed", "0 | 0 passed"),
                        body.replace("test_lubancore_lua.cpp", "test_other.cpp"),
                        body.replace("lubancore_sdk_tests.exe", "another.exe"),
                        body.replace("Test Passed.", "Test Failed.")):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                focused.check_lua_native(changed)

    def test_missing_duplicate_or_foreign_marker_cannot_prove_a_path(self):
        body = self.body()
        for path in focused.LUA_PATHS:
            marker = "[sdk-lua-path] " + path
            for changed in (body.replace(marker, ""), body.replace(marker, marker + "\n" + marker),
                            body.replace(marker, "different-source: " + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_lua_native(changed)


if __name__ == "__main__":
    unittest.main()
