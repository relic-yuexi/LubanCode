"""Parser counterexamples only; synthetic logs are never native evidence."""
import unittest

from scripts.ci import sdk_model_input as gate


def argv(stem, exe, windows=False):
    return [('C:/data/' if windows else '/data/') + exe + ('.exe' if windows else ''),
            '--source-file=*test_' + stem + '.cpp']


def section(stem, command):
    count, prefix, paths = gate.SOURCES[stem]
    return '\n'.join(['Command: ' + ' '.join('"' + arg + '"' for arg in command),
                      *(prefix + path for path in paths),
                      f'[doctest] test cases: {count} | {count} passed | 0 failed | 1800 skipped',
                      '[doctest] assertions: 50 | 50 passed | 0 failed', 'Test Passed.']) + '\n'


class ModelInputGateTests(unittest.TestCase):
    def test_both_native_platforms_and_real_executable_rosters(self):
        for windows in (False, True):
            for stem in gate.SOURCES:
                executables = ('lubancode_tests',) if stem == 'model_input_wrappers' else ('lubancore_sdk_tests', 'lubancode_tests')
                for exe in executables:
                    command = argv(stem, exe, windows)
                    self.assertEqual(gate.check_native(section(stem, command), command, stem)['nativeCases'], gate.SOURCES[stem][0])

    def test_registration_rejects_selected_case_extra_argv_foreign_and_relative(self):
        command = argv('lubancore_model_input', 'lubancore_sdk_tests')
        for changed in (None, [], [None, command[1]], command[:1], [*command, '--test-case=one'],
                        ['lubancore_sdk_tests', command[1]], [command[0], '--source-file=*model*'],
                        ['/data/lubancode_tests', command[1]], [command[0], '--source-file=*test_other.cpp']):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                gate.check_registration(changed, 'lubancore_model_input')
        with self.assertRaises(RuntimeError):
            gate.check_registration(argv('model_input_wrappers', 'lubancore_sdk_tests'), 'model_input_wrappers')

    def test_original_command_and_completion_are_required_once(self):
        stem = 'lubancore_model_input'; command = argv(stem, 'lubancore_sdk_tests'); text = section(stem, command)
        for changed in (text.replace(command[0], '/other/lubancore_sdk_tests'), text + 'Command: other\n',
                        text.replace('Test Passed.', 'Test Failed.'), text + 'Test Passed.\n',
                        text.replace('Test Passed.', 'Test Not Run.'), text + 'SKIPPED: native unavailable\n'):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                gate.check_native(changed, command, stem)

    def test_empty_failed_duplicated_and_partial_native_are_rejected(self):
        for stem, (count, prefix, paths) in gate.SOURCES.items():
            command = argv(stem, 'lubancode_tests'); text = section(stem, command)
            for changed in (text.replace(f'{count} | {count} passed', f'{count - 1} | {count - 1} passed'),
                            text.replace('50 | 50 passed', '0 | 0 passed'), text.replace('50 | 50 passed', '50 | 49 passed'),
                            text + '[doctest] assertions: 1 | 1 passed | 0 failed\n',
                            text.replace(prefix + paths[0] + '\n', ''), text + prefix + paths[0] + '\n',
                            text + prefix + 'foreign\n'):
                with self.subTest(stem=stem), self.assertRaises(RuntimeError):
                    gate.check_native(changed, command, stem)

    def test_cross_source_witness_is_rejected(self):
        for stem in gate.SOURCES:
            other = next(name for name in gate.SOURCES if name != stem)
            command = argv(stem, 'lubancode_tests')
            text = section(stem, command) + gate.SOURCES[other][1] + gate.SOURCES[other][2][0] + '\n'
            with self.assertRaises(RuntimeError):
                gate.check_native(text, command, stem)


if __name__ == '__main__':
    unittest.main()
