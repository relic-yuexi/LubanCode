"""Reject incomplete or substituted allocation qualification receipts; no native work."""
import unittest
import json
from scripts.ci.check_sdk_focused import check_allocation_owner_native


class AllocationQualificationGateTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_allocation_owner.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 1 | 1 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[sdk-allocation-owner-route] %d %d %d %d' % (kind, sized, size, route)
              for kind in range(8) for sized in (0, 1) for size in (0, 1, 257) for route in range(3)),
            '[sdk-allocation-owner] qualified_paths=144 image=/actual build/liblubancore.so',
            *('[sdk-module-refusal] form=%d baseline=1 refused=1 host=0' % kind for kind in range(8)),
            '[sdk-module-refusal] qualified_forms=8 imports_restored=1',
            *('[sdk-returned-allocation-baseline] path=%d allocations=%d refusals=0 retired=1 recovery_calls=0' % (path, 101 + path) for path in range(3)),
            *('[sdk-returned-allocation-point] path=%d ordinal=%d allocations=%d phase=2 refusals=1 retired=1 recovery_calls=0 state=%d persisted=1 finals=1 observations=1 terminals=%d' % (path, ordinal, 101 + path, 2 if path == 0 else 4, int(path != 2))
              for path in range(3) for ordinal in range(1, 102 + path)),
            *('[sdk-returned-allocation-sweep] path=%d baseline=%d points=%d' % (path, 101 + path, 101 + path) for path in range(3)),
            *('[sdk-returned-allocation-materials] path=%d ordinal=%d directory=%s' % (path, ordinal, json.dumps('/actual build/allocation-matrix/%d-%d/session' % (path, ordinal)))
              for path in range(3) for ordinal in range(102 + path))))

    def test_native_commands_and_line_endings(self):
        for executable in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe', '/real build/lubancode_tests'):
            command = [executable, self.command[1]]
            for ending in ('\n', '\r\n'):
                value = check_allocation_owner_native(self.body(command).replace('\n', ending), command)
                self.assertEqual(value['qualified_paths'], 144)
                self.assertEqual(value['allocation_refusals'], 8)
                self.assertEqual(value['refusal_scope'], 'module_port_qualification_and_actual_session_sweep')
                self.assertEqual(value['ordinary_session_allocation_matrix'], 'swept')
                self.assertEqual(value['returned_session_baselines'], {'0': 101, '1': 102, '2': 103})

    def test_each_route_must_appear_once(self):
        for kind in range(8):
            for sized in (0, 1):
                for size in (0, 1, 257):
                    for route in range(3):
                        marker = '[sdk-allocation-owner-route] %d %d %d %d' % (kind, sized, size, route)
                        for body in (self.body().replace(marker + '\n', ''), self.body() + '\n' + marker):
                            with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                                check_allocation_owner_native(body, self.command)

    def test_foreign_route_cannot_replace_a_required_route(self):
        for wrong in ('8 0 0 0', '0 0 2 0', '0 0 0 3'):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(self.body().replace('route] 0 0 0 0', 'route] ' + wrong), self.command)

    def test_summary_and_actual_image_are_required_once(self):
        marker = '[sdk-allocation-owner] qualified_paths=144 image=/actual build/liblubancore.so'
        for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                     self.body().replace('paths=144', 'paths=143'), self.body().replace('image=/actual build/liblubancore.so', 'image= ')):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(body, self.command)

    def test_failed_native_case_cannot_pass(self):
        for old, new in (('1 | 1 passed', '0 | 0 passed'), ('1 passed | 0 failed', '0 passed | 1 failed'), ('Test Passed.', 'Test Failed.')):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(self.body().replace(old, new), self.command)

    def test_different_source_cannot_substitute(self):
        command = [self.command[0], '--source-file=*test_lubancore_lifecycle.cpp']
        with self.assertRaises(RuntimeError):
            check_allocation_owner_native(self.body(command), command)

    def test_each_real_module_refusal_is_required_once(self):
        for kind in range(8):
            marker = '[sdk-module-refusal] form=%d baseline=1 refused=1 host=0' % kind
            for body in (self.body().replace(marker + '\n', ''), self.body() + '\n' + marker):
                with self.subTest(kind=kind), self.assertRaises(RuntimeError):
                    check_allocation_owner_native(body, self.command)

    def test_refusal_requires_real_baseline_fault_and_host_isolation(self):
        marker = '[sdk-module-refusal] form=0 baseline=1 refused=1 host=0'
        for wrong in ('form=8 baseline=1 refused=1 host=0', 'form=0 baseline=0 refused=1 host=0',
                      'form=0 baseline=1 refused=0 host=0', 'form=0 baseline=1 refused=2 host=0',
                      'form=0 baseline=1 refused=1 host=1'):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(self.body().replace(marker, '[sdk-module-refusal] ' + wrong), self.command)

    def test_refusal_import_restore_and_marker_shape_are_required(self):
        marker = '[sdk-module-refusal] qualified_forms=8 imports_restored=1'
        for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                     self.body().replace('imports_restored=1', 'imports_restored=0'),
                     self.body() + '\n[sdk-module-refusal] invalid'):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(body, self.command)

    def test_each_returned_session_path_is_required_once(self):
        for path in range(3):
            marker = '[sdk-returned-allocation-baseline] path=%d allocations=%d refusals=0 retired=1 recovery_calls=0' % (path, 101 + path)
            for body in (self.body().replace(marker, ''), self.body() + '\n' + marker):
                with self.assertRaises(RuntimeError):
                    check_allocation_owner_native(body, self.command)

    def test_baseline_requires_allocation_retirement_and_zero_replay(self):
        marker = '[sdk-returned-allocation-baseline] path=0 allocations=101 refusals=0 retired=1 recovery_calls=0'
        for wrong in (marker.replace('allocations=101', 'allocations=0'), marker.replace('refusals=0', 'refusals=1'),
                      marker.replace('retired=1', 'retired=0'), marker.replace('recovery_calls=0', 'recovery_calls=1'),
                      marker.replace('path=0', 'path=3')):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(self.body().replace(marker, wrong), self.command)

    def test_malformed_extra_baseline_cannot_pass(self):
        with self.assertRaises(RuntimeError):
            check_allocation_owner_native(self.body() + '\n[sdk-returned-allocation-baseline] invalid', self.command)

    def test_each_refusal_point_is_required_once(self):
        for path in range(3):
            for ordinal in (1, 101 + path):
                marker = next(line for line in self.body().splitlines() if line.startswith('[sdk-returned-allocation-point] path=%d ordinal=%d ' % (path, ordinal)))
                for body in (self.body().replace(marker + '\n', ''), self.body() + '\n' + marker):
                    with self.assertRaises(RuntimeError):
                        check_allocation_owner_native(body, self.command)

    def test_refusal_point_requires_actual_phase_count_and_confirmation(self):
        marker = next(line for line in self.body().splitlines() if line.startswith('[sdk-returned-allocation-point] path=0 ordinal=101 '))
        for wrong in (marker.replace('allocations=101', 'allocations=100'), marker.replace('phase=2', 'phase=0'),
                      marker.replace('phase=2', 'phase=10'), marker.replace('refusals=1', 'refusals=0'),
                      marker.replace('retired=1', 'retired=0'), marker.replace('recovery_calls=0', 'recovery_calls=1'),
                      marker.replace('state=2', 'state=5'), marker.replace('persisted=1', 'persisted=0'),
                      marker.replace('finals=1', 'finals=0')):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(self.body().replace(marker, wrong), self.command)

    def test_sweep_summary_cannot_truncate_or_replace_a_path(self):
        marker = '[sdk-returned-allocation-sweep] path=0 baseline=101 points=101'
        for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                     self.body().replace(marker, marker.replace('points=101', 'points=100')),
                     self.body().replace(marker, marker.replace('baseline=101', 'baseline=100'))):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(body, self.command)

    def test_each_original_material_directory_is_required_and_distinct(self):
        marker = next(line for line in self.body().splitlines() if line.startswith('[sdk-returned-allocation-materials] path=0 ordinal=101 '))
        for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                     self.body().replace(marker, marker.replace('0-101/session', '0-100/session')),
                     self.body().replace(marker, marker.split('directory=')[0] + 'directory=""')):
            with self.assertRaises(RuntimeError):
                check_allocation_owner_native(body, self.command)
