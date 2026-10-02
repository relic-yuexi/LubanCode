"""Pure-data checks: evidence omissions must not turn a native failure green."""
import importlib.util
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

spec = importlib.util.spec_from_file_location("workspace_read_gate", Path(__file__).resolve().parents[1] / "check_workspace_manifest_reads.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def sample(platform="windows-msvc"):
    registration = {"tests": [{"name": name, "command": ["native-tests", "--source-file=" + file]} for name, file in gate.SOURCES.items()]}
    junit = ET.Element("testsuite")
    logs = []
    for number, name in enumerate(gate.SOURCES, 1):
        ET.SubElement(junit, "testcase", name=name, status="run")
        count = (13 if platform == "windows-msvc" else 12) if name.startswith("unit.") else (5 if platform == "windows-msvc" else 4)
        logs.append(f"test {number}\n    Start {number}: {name}\n{number}: [doctest] test cases: {count} | {count} passed | 0 failed\n{number}: [doctest] assertions: 30 | 30 passed | 0 failed\n")
    return registration, junit, "".join(logs), platform


class WorkspaceManifestReadGateTests(unittest.TestCase):
    def test_success_for_each_platform(self):
        for platform in ("windows-msvc", "macos-clang", "linux-manylinux"):
            with self.subTest(platform=platform):
                self.assertEqual(len(gate.validate(*sample(platform))), 2)

    def test_disabled_source_rejected(self):
        registration, *rest = sample()
        registration["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
        with self.assertRaises(ValueError):
            gate.validate(registration, *rest)

    def test_wrong_source_filter_rejected(self):
        registration, *rest = sample()
        registration["tests"][0]["command"] = ["native-tests", "--source-file=unrelated.cpp"]
        with self.assertRaises(ValueError):
            gate.validate(registration, *rest)

    def test_skipped_or_failed_junit_rejected(self):
        for status in ("skipped", "failure"):
            registration, junit, native, platform = sample()
            ET.SubElement(junit[0], status)
            with self.subTest(status=status), self.assertRaises(ValueError):
                gate.validate(registration, junit, native, platform)

    def test_native_zero_cases_rejected(self):
        registration, junit, native, platform = sample()
        native = native.replace("13 | 13 passed | 0 failed", "0 | 0 passed | 0 failed")
        with self.assertRaises(ValueError):
            gate.validate(registration, junit, native, platform)

    def test_native_failed_assertion_rejected(self):
        registration, junit, native, platform = sample()
        native = native.replace("30 | 30 passed | 0 failed", "30 | 29 passed | 1 failed", 1)
        with self.assertRaises(ValueError):
            gate.validate(registration, junit, native, platform)

    def test_omitted_native_source_rejected(self):
        registration, junit, native, platform = sample()
        native = native.split("test 2", 1)[0]
        with self.assertRaises(ValueError):
            gate.validate(registration, junit, native, platform)


if __name__ == "__main__":
    unittest.main()
