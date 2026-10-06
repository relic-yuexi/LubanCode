#!/usr/bin/env python3
"""Preserve the existing full run's OwnedFilePaths original/focused evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ET

try:
    from . import sdk_owned_file_paths as owned
except ImportError:
    import sdk_owned_file_paths as owned

REQUIRED = {'sdk.focused.' + owned.STEM: 'lubancore_sdk_tests',
            'integration.sdk.' + owned.STEM: 'lubancode_tests'}


def check_registration(raw, build):
    value = json.loads(raw, object_pairs_hook=owned.commands.unique_object)
    tests = value.get('tests') if isinstance(value, dict) else None
    owned.require(isinstance(tests, list) and len(tests) == 2 and all(isinstance(t, dict) for t in tests) and
                  {t.get('name') for t in tests} == set(REQUIRED), 'full registration pair is missing, duplicated or foreign')
    commands = {}
    for test in tests:
        name = test['name']; command = test.get('command')
        owned.check_registration(command, REQUIRED[name])
        owned.require(Path(command[0]).resolve().is_relative_to(Path(build).resolve()), 'full executable left its actual build')
        properties = test.get('properties')
        owned.require(isinstance(properties, list) and all(isinstance(p, dict) and isinstance(p.get('name'), str) and 'value' in p for p in properties),
                      'full properties malformed')
        props = {p['name']: p['value'] for p in properties}
        owned.require(len(props) == len(properties) and not props.get('DISABLED') and
                      type(props.get('TIMEOUT')) in (int, float) and props['TIMEOUT'] == 300,
                      'full source disabled or original300s timeout changed')
        commands[name] = command
    return commands


def extract(build, platform_name, source):
    build = Path(build).resolve(); source = Path(source).resolve()
    owned.require(platform_name in ('nt', 'posix'), 'unknown full-run platform')
    output = build / 'test-evidence/owned-file-paths-full'
    owned.require(output.resolve().is_relative_to(build), 'derived evidence escaped its build')
    output.mkdir(parents=True, exist_ok=True)
    for name in ('context.json', 'registration.json', 'results.xml', 'LastTest.log', owned.SOURCE_COPY, owned.SOURCE_RECORD):
        (output / name).unlink(missing_ok=True)
    inputs = {'registration.json': build / 'owned-file-paths-full-registration.json',
              'results.xml': build / 'result-store-full-results.xml',
              'LastTest.log': build / 'Testing/Temporary/LastTest.log'}
    report = {'githubSha': os.environ.get('GITHUB_SHA'), 'platform': platform_name, 'requiredTests': sorted(REQUIRED),
              'status': 'collecting', 'inputs': {},
              'scope': 'Original/focused pair from the existing full run; overall full CI remains independently gated'}
    raw = {}

    def save():
        (output / 'context.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    save()
    try:
        # Preserve every available original before parsing any one of them.
        for name, path in inputs.items():
            try:
                raw[name] = path.read_bytes()
                report['inputs'][name] = {'path': str(path), 'bytes': len(raw[name]), 'sha256': hashlib.sha256(raw[name]).hexdigest()}
                (output / name).write_bytes(raw[name])
            except OSError as error:
                report['inputs'][name] = {'path': str(path), 'error': str(error)}
        save()
        record = owned.capture_source(source, output)
        report['githubSha'] = record['checkoutHead']; report['source'] = record
        save()
        owned.require(set(raw) == set(inputs), 'same full-run original input missing')
        commands = check_registration(raw['registration.json'], build)
        root = ET.fromstring(raw['results.xml'])
        cases = [c for c in root.findall('.//testcase') if c.attrib.get('name') in REQUIRED]
        owned.require(len(cases) == 2 and {c.attrib['name'] for c in cases} == set(REQUIRED), 'full JUnit pair missing or duplicated')
        for case in cases:
            owned.require(case.attrib.get('status') == 'run' and all(case.find(k) is None for k in ('failure', 'error', 'skipped')),
                          'full JUnit source failed or skipped')
        sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$', raw['LastTest.log'].decode('utf-8'), flags=re.M)
        details = {}
        source_bytes = (output / owned.SOURCE_COPY).read_bytes()
        for name in REQUIRED:
            matches = [sections[i + 1] for i in range(1, len(sections), 2) if sections[i] == name]
            owned.require(len(matches) == 1, 'full native section missing or duplicated: ' + name)
            details[name] = owned.check_native(matches[0], commands[name], record, source_bytes, report['githubSha'], platform_name)
        report.update(status='passed', details=details); save()
        return report
    except Exception as error:
        report.update(status='failed', error=str(error)); save()
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, default=Path.cwd())
    parser.add_argument('--platform', choices=('nt', 'posix'), required=True)
    args = parser.parse_args()
    print(json.dumps(extract(args.build_dir, args.platform, args.source_dir), ensure_ascii=True))


if __name__ == '__main__':
    main()
