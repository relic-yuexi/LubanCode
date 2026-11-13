"""Pure full-run evidence counterexamples; no shell or native calls."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

CI = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("command_limits_full", CI / "extract_command_limits_full.py")
gate = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(CI), *sys.path]):
    spec.loader.exec_module(gate)


def native_body(name, command, platform="posix"):
    shells = ("cmd", "powershell") if platform == "nt" else ("sh", "bash")
    records = [json.dumps({"shell": shell, "cwd": "/actual/cwd", "exact_command": "probe exact 128",
        "excess_command": "probe excess 129", "timeout_ms": 15000, "max_output_bytes": 128,
        "exact_request_bytes": 128, "excess_request_bytes": 129, "exact_outcome": "succeeded",
        "excess_error_code": "process.output_limit"}) for shell in shells]
    return (f'1/3 Testing: {name}\nCommand: "{command[0]}" "{command[1]}"\n' +
        '[doctest] test cases: 6 | 6 passed | 0 failed | 900 skipped\n' +
        '[doctest] assertions: 101 | 101 passed | 0 failed\n' +
        ''.join('[command-limits-path] ' + name + '\n' for name in gate.COMMAND_LIMITS_PATHS) +
        ''.join('[command-limits-shell] ' + record + '\n' for record in records) + 'Test Passed.\n')


def materialize(build, platform="posix"):
    tests, bodies = [], {}
    for name, (binary, timeout) in gate.REQUIRED.items():
        command = [str(build / "tests" / (binary + (".exe" if platform == "nt" else ""))).replace("\\", "/"), gate.FILTER]
        tests.append({"name": name, "command": command, "properties": [{"name": "TIMEOUT", "value": timeout}]})
        if platform == "nt":
            tests[-1]["properties"].append({"name": "RUN_SERIAL", "value": True})
        bodies[name] = native_body(name, command, platform)
    (build / "command-limits-full-registration.json").write_text(json.dumps({"tests": tests}), encoding="utf-8")
    (build / "Testing/Temporary").mkdir(parents=True)
    (build / "Testing/Temporary/LastTest.log").write_text(''.join(bodies.values()) +
        '3/3 Testing: unit.foreign.source\nforeign-native-must-not-upload\n', encoding="utf-8")
    (build / "result-store-full-results.xml").write_text('<testsuite>' + ''.join(
        f'<testcase name="{name}" status="run"/>' for name in gate.REQUIRED) +
        '<testcase name="unit.foreign.source" status="run"><system-out>foreign-junit-must-not-upload</system-out></testcase></testsuite>',
        encoding="utf-8")
    return tests, bodies


class CommandLimitsFullGateTests(unittest.TestCase):
    def check_manifest(self, build, status):
        output = build / "test-evidence/command-limits-full"
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["status"], status)
        self.assertNotIn("manifest.json", manifest["files"])
        for name, facts in manifest["files"].items():
            data = (output / name).read_bytes()
            self.assertEqual(facts["bytes"], len(data))
            self.assertEqual(facts["sha256"], hashlib.sha256(data).hexdigest())
        return output

    def test_actual_pair_and_platform_shells_pass_with_owned_scoped_originals(self):
        for platform in ("nt", "posix"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(prefix="limits full ") as directory:
                build = Path(directory)
                tests, bodies = materialize(build, platform)
                report = gate.extract(build, platform)
                self.assertEqual(report["status"], "passed")
                self.assertEqual(report["stage"], "complete")
                for test in tests:
                    self.assertEqual(report["details"][test["name"]], {"command": test["command"],
                        "nativeCases": 6, "nativeAssertions": 101})
                output = self.check_manifest(build, "passed")
                self.assertEqual((output / "LastTest.log").read_text(encoding="utf-8"), ''.join(bodies.values()))
                self.assertNotIn("foreign-junit", (output / "results.xml").read_text(encoding="utf-8"))
                self.assertEqual(report["sourceSha256"], hashlib.sha256((CI.parents[1] / gate.SOURCE).read_bytes()).hexdigest())

    def test_registration_requires_absolute_exact_two_string_argv(self):
        name = next(iter(gate.REQUIRED))
        valid = ["C:/actual build/lubancore_sdk_tests.exe", gate.FILTER]
        self.assertEqual(gate.check_registration(name, [valid[0].replace('/', '\\'), gate.FILTER]), valid)
        for command in (None, {}, "native", (), tuple(valid), [], [None, gate.FILTER], [3, gate.FILTER],
                        [valid[0], 3], valid[:1], ["lubancore_sdk_tests.exe", gate.FILTER],
                        ["C:lubancore_sdk_tests.exe", gate.FILTER], ["/build/lubancode_tests", gate.FILTER],
                        [valid[0], "--source-file=*other.cpp"], [*valid, "--test-case=one"],
                        [valid[0] + "\0", gate.FILTER], [valid[0], gate.FILTER + "\n"]):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                gate.check_registration(name, command)
        for name in ("unit.foreign.source", None, {}, []):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                gate.check_registration(name, valid)

    def test_actual_command_missing_duplicate_foreign_quote_and_extra_args_reject(self):
        name = next(iter(gate.REQUIRED)); command = ["/actual/build/lubancore_sdk_tests", gate.FILTER]
        body = native_body(name, command)
        line = f'Command: "{command[0]}" "{command[1]}"\n'
        for changed in ("", line * 2, line.replace('/actual/', '/foreign/'),
                        'Command: "unterminated\n', line.rstrip() + ' "--no-skip=true"\n',
                        line.replace(gate.FILTER, "--source-file=*other.cpp")):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                gate.check_source_native(name, body.replace(line, changed), "posix", command)
        self.assertEqual(gate.check_source_native(name, body.replace('\n', '\r\n'), "posix", command)["nativeCases"], 6)

    def test_roster_assertions_six_markers_and_real_shell_records_cannot_be_substituted(self):
        name = next(iter(gate.REQUIRED)); command = ["/actual/lubancore_sdk_tests", gate.FILTER]
        body = native_body(name, command)
        variants = (body.replace('6 | 6 passed', '5 | 5 passed'), body.replace('6 | 6 passed', '0 | 0 passed'),
            body.replace('101 | 101 passed', '0 | 0 passed'), body.replace('101 | 101 passed', '101 | 100 passed'),
            body + '[doctest] assertions: 101 | 101 passed | 0 failed\n',
            body.replace('[command-limits-path] timeout\n', ''), body + '[command-limits-path] cancel\n',
            body.replace('"exact_outcome": "succeeded"', '"exact_outcome": "timed_out"'),
            body.replace('"max_output_bytes": 128', '"max_output_bytes": true'),
            body.replace('"bash"', '"powershell"'), body.replace('[command-limits-shell] {', '[command-limits-shell] invalid{'))
        for changed in variants:
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                gate.check_source_native(name, changed, "posix", command)

    def test_failure_timeout_and_abort_originals_are_retained_before_rejection(self):
        for failure in ('Test Failed.\n', 'Test Timeout.\n', 'Subprocess aborted\n',
                        'Test Passed.\nTest Failed.\n', 'Test Passed.\nTest Passed.\n', 'SKIP: source\nTest Passed.\n'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                build = Path(directory); _, bodies = materialize(build)
                raw = ''.join(bodies.values()).replace('Test Passed.\n', failure, 1).encode()
                (build / "Testing/Temporary/LastTest.log").write_bytes(raw)
                with self.assertRaises(RuntimeError): gate.extract(build, "posix")
                output = self.check_manifest(build, "failed")
                self.assertEqual((output / "LastTest.log").read_bytes(), raw)

    def test_junit_failure_error_skip_and_notrun_cannot_borrow_native_success(self):
        for kind, status in (("failure", "run"), ("error", "run"), ("skipped", "run"), (None, "notrun")):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                build = Path(directory); _, bodies = materialize(build)
                first, second = gate.REQUIRED
                child = f'<{kind} message="original failure"/>' if kind else ''
                xml = f'<testsuite><testcase name="{first}" status="{status}">{child}</testcase><testcase name="{second}" status="run"/></testsuite>'
                (build / "result-store-full-results.xml").write_text(xml, encoding="utf-8")
                with self.assertRaises(RuntimeError): gate.extract(build, "posix")
                output = self.check_manifest(build, "failed")
                self.assertEqual((output / "LastTest.log").read_text(encoding="utf-8"), ''.join(bodies.values()))
                retained = ET.parse(output / "results.xml").getroot().find("testcase")
                self.assertEqual(retained.attrib["status"], status)
                if kind:
                    self.assertEqual(retained.find(kind).attrib["message"], "original failure")

    def test_missing_duplicate_or_foreign_source_pair_in_each_input_rejects(self):
        for field in ("registration", "native", "junit"):
            for change in ("missing", "duplicate", "foreign"):
                with self.subTest(field=field, change=change), tempfile.TemporaryDirectory() as directory:
                    build = Path(directory); tests, bodies = materialize(build); first, second = gate.REQUIRED
                    if field == "registration":
                        if change == "missing": tests.pop()
                        elif change == "duplicate": tests[1] = tests[0]
                        else: tests[1]["name"] = "unit.foreign.source"
                        (build / "command-limits-full-registration.json").write_text(json.dumps({"tests": tests}), encoding="utf-8")
                    elif field == "native":
                        raw = bodies[first] + (bodies[first] if change == "duplicate" else bodies[second].replace(second, 'unit.foreign.source') if change == "foreign" else '')
                        (build / "Testing/Temporary/LastTest.log").write_text(raw, encoding="utf-8")
                    else:
                        names = [first] if change == "missing" else [first, first if change == "duplicate" else "unit.foreign.source"]
                        (build / "result-store-full-results.xml").write_text('<testsuite>' + ''.join(
                            f'<testcase name="{name}" status="run"/>' for name in names) + '</testsuite>', encoding="utf-8")
                    with self.assertRaises(RuntimeError): gate.extract(build, "posix")
                    self.check_manifest(build, "failed")

    def test_foreign_registration_build_swapped_owner_properties_and_shapes_reject(self):
        for change in ("foreign-build", "swapped-owner", "timeout", "bool-timeout", "disabled", "duplicate-property", "bad-properties", "bad-shape", "bad-name"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                build = Path(directory); tests, _ = materialize(build); test = tests[0]
                if change == "foreign-build": test["command"][0] = '/foreign/build/lubancore_sdk_tests'
                elif change == "swapped-owner": test["command"][0] = str(build / 'tests/lubancode_tests')
                elif change == "timeout": test["properties"][0]["value"] = 999
                elif change == "bool-timeout": test["properties"][0]["value"] = True
                elif change == "disabled": test["properties"].append({"name": "DISABLED", "value": True})
                elif change == "duplicate-property": test["properties"].append(test["properties"][0])
                elif change == "bad-properties": test["properties"] = None
                elif change == "bad-name": test["name"] = []
                data = tests if change == "bad-shape" else {"tests": tests}
                (build / "command-limits-full-registration.json").write_text(json.dumps(data), encoding="utf-8")
                with self.assertRaises(RuntimeError): gate.extract(build, "posix")
                self.check_manifest(build, "failed")

    def test_windows_both_actual_registrations_require_true_run_serial_and_keep_originals(self):
        for selected in range(2):
            for value in (None, False, 0, 1, "TRUE", "false", [], {}):
                with self.subTest(selected=selected, value=value), tempfile.TemporaryDirectory() as directory:
                    build = Path(directory); tests, bodies = materialize(build, "nt")
                    properties = tests[selected]["properties"]
                    properties[:] = [item for item in properties if item["name"] != "RUN_SERIAL"]
                    if value is not None:
                        properties.append({"name": "RUN_SERIAL", "value": value})
                    raw = json.dumps({"tests": tests}).encode()
                    (build / "command-limits-full-registration.json").write_bytes(raw)
                    with self.assertRaisesRegex(RuntimeError, 'isolate external CTest load'):
                        gate.extract(build, "nt")
                    output = self.check_manifest(build, "failed")
                    self.assertEqual((output / 'registration.json').read_bytes(), raw)
                    self.assertEqual((output / 'LastTest.log').read_text(encoding='utf-8'), ''.join(bodies.values()))

    def test_posix_registration_has_no_new_serial_requirement(self):
        for value in (None, False, True):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                build = Path(directory); tests, _ = materialize(build)
                if value is not None:
                    for test in tests: test['properties'].append({'name': 'RUN_SERIAL', 'value': value})
                (build / 'command-limits-full-registration.json').write_text(json.dumps({'tests': tests}), encoding='utf-8')
                self.assertEqual(gate.extract(build, 'posix')['status'], 'passed')

    def test_missing_inputs_keep_available_bytes_and_failed_manifest(self):
        for name in ("command-limits-full-registration.json", "result-store-full-results.xml", "Testing/Temporary/LastTest.log"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                build = Path(directory); materialize(build); (build / name).unlink()
                with self.assertRaisesRegex(RuntimeError, 'input is missing'): gate.extract(build, "posix")
                output = self.check_manifest(build, "failed")
                if name != "Testing/Temporary/LastTest.log": self.assertTrue((output / "LastTest.log").is_file())

    def test_bad_xml_retains_exact_unscoped_original_and_native_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory); _, bodies = materialize(build)
            raw = b'<testsuite>\r\n<testcase incomplete\xff'
            (build / "result-store-full-results.xml").write_bytes(raw)
            with self.assertRaisesRegex(RuntimeError, 'JUnit XML is invalid'): gate.extract(build, "posix")
            output = self.check_manifest(build, "failed")
            self.assertEqual((output / "junit-unparsed.xml").read_bytes(), raw)
            self.assertEqual((output / "LastTest.log").read_text(encoding='utf-8'), ''.join(bodies.values()))
            self.assertEqual(json.loads((output / 'context.json').read_text())['junitScope'], 'unparsed-original')

    def test_no_native_scope_retains_original_without_claiming_a_source(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory); materialize(build)
            raw = b'CTest abort before first source header\r\n\xff\n'
            (build / "Testing/Temporary/LastTest.log").write_bytes(raw)
            with self.assertRaises(RuntimeError): gate.extract(build, "posix")
            output = self.check_manifest(build, "failed")
            self.assertEqual((output / "native-unscoped.log").read_bytes(), raw)
            self.assertEqual(json.loads((output / 'context.json').read_text())['nativeSections'], [])

    def test_fresh_collection_removes_only_own_stale_derived_files(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory); materialize(build)
            output = build / "test-evidence/command-limits-full"; output.mkdir(parents=True)
            for name in ('junit-unparsed.xml', 'native-unscoped.log'): (output / name).write_bytes(b'stale pass')
            (output / 'foreign-evidence.json').write_bytes(b'owned elsewhere')
            report = gate.extract(build, "posix")
            self.assertEqual(report['status'], 'passed')
            self.assertFalse((output / 'junit-unparsed.xml').exists())
            self.assertFalse((output / 'native-unscoped.log').exists())
            self.assertEqual((output / 'foreign-evidence.json').read_bytes(), b'owned elsewhere')


class CommandLimitsSchedulingWiringTests(unittest.TestCase):
    def test_windows_cli_and_sdk_registration_sites_are_independently_scoped(self):
        # Inspect only this narrow CMake addition. Actual remote registration is
        # checked above, so a text match cannot serve as native scheduling proof.
        root = CI.parents[1]
        sites = (
            ('tests/CMakeLists.txt', 'foreach(test_entry IN LISTS LUBANCODE_TEST_ENTRIES)',
             'test_name', 'unit.tools.run_command_execution_limits', 'test_name'),
            ('cmake/LubanCoreTests.cmake', 'foreach(sdk_source IN LISTS LUBANCORE_FOCUSED_TEST_SOURCES)',
             'sdk_basename', 'test_run_command_execution_limits.cpp', 'sdk_test'),
        )
        for filename, loop, selector, value, target in sites:
            text = (root / filename).read_text(encoding='utf-8')
            body = text.split(loop, 1)[1].split('endforeach()', 1)[0]
            block = (f'if(WIN32 AND {selector} STREQUAL "{value}")\n'
                     f'    set_tests_properties("${{{target}}}" PROPERTIES RUN_SERIAL TRUE)\n'
                     '  endif()')
            with self.subTest(filename=filename):
                self.assertEqual(body.count(block), 1)
                self.assertEqual(body.count('RUN_SERIAL'), 1)
                self.assertLess(body.index('add_test('), body.index(block))
                self.assertNotIn('LUBANCODE_BUILD_SDK', body)
        root_tests = (root / 'tests/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertLess(root_tests.index('foreach(test_entry IN LISTS LUBANCODE_TEST_ENTRIES)'),
                        root_tests.index('# Reuse the SDK test graph'))

    def test_windows_full_original_upload_follows_actual_test_and_keeps_scoped_gates(self):
        workflow = (CI.parents[1] / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        job = workflow.split('  build-test:\n', 1)[1].split('  linux-manylinux:\n', 1)[0]
        body = job.split('      - name: Test\n', 1)[1].split('      - name: ', 1)[0]
        capture = ('if [ \'${{ matrix.name }}\' = windows-msvc ]; then\n'
                   '              ctest --test-dir build -C Release "${PROCESS_FILTER[@]}" --show-only=json-v1 > build/windows-full-test-registration.json\n'
                   '            fi')
        actual = 'ctest --test-dir build -C Release "${PROCESS_FILTER[@]}" --parallel 4 --output-on-failure --no-tests=error --output-junit "$PWD/build/result-store-full-results.xml"'
        self.assertEqual(body.count(capture), 1)
        self.assertLess(body.index(capture), body.index(actual))
        self.assertEqual(body.count(actual), 1)
        upload = job.split('      - name: Upload Windows full Test originals\n', 1)[1].split('      - name: ', 1)[0]
        self.assertIn("if: always() && matrix.name == 'windows-msvc' && inputs.test_filter == '' && (steps.test.outcome == 'success' || steps.test.outcome == 'failure')", upload)
        self.assertIn('uses: actions/upload-artifact@v4', upload)
        self.assertIn('name: full-test-originals-windows-msvc', upload)
        for path in ('build/windows-full-test-registration.json', 'build/result-store-full-results.xml',
                     'build/Testing/Temporary/LastTest.log'):
            self.assertEqual(upload.count(path), 1)
        self.assertIn('if-no-files-found: error', upload)
        for scope in ('command-limits', 'child-history', 'agent-health', 'result-store'):
            self.assertRegex(job, r'      - name: Extract actual full ' + re.escape(scope))


if __name__ == "__main__":
    unittest.main()
