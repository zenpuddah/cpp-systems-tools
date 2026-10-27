"""Pure evidence validation failure tests; no network or fabricated pass records."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('import_memory_ci', Path(__file__).resolve().parents[1] / 'scripts/import_memory_ci.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
audit_spec = importlib.util.spec_from_file_location('public_audit', Path(__file__).resolve().parents[1] / 'scripts/public_audit.py')
audit = importlib.util.module_from_spec(audit_spec)
audit_spec.loader.exec_module(audit)
report_spec = importlib.util.spec_from_file_location('inspector_report', Path(__file__).resolve().parents[1] / 'projects/runtime-inspector/report.py')
report = importlib.util.module_from_spec(report_spec)
report_spec.loader.exec_module(report)


class EvidenceValidation(unittest.TestCase):
    def setUp(self):
        self.fixture = {'extension': 'memory-v1', 'platform': 'Linux-test-fixture', 'machine': 'x86_64',
                        'revision': {'output': 'fixture-revision\n'}, 'source_drift': False,
                        'source': {'sha256': 'fixture-only'}, 'source_after': {'sha256': 'fixture-only'},
                        'status': 'passed', 'presets': {'dev': [
                            {'status': 'passed', 'exit_code': 0}, {'status': 'passed', 'exit_code': 0},
                            {'status': 'passed', 'exit_code': 0, 'tests': {'status': 'passed', 'count': 1}}]}}

    def test_complete_fixture(self):
        self.assertEqual(module.validate(self.fixture, 'dev', 'fixture-revision'), 'passed')

    def test_scoped_public_evidence_allowlist(self):
        self.assertTrue(audit.allowed('evidence/memory-v1/native/run/dev-tests.xml'))
        self.assertTrue(audit.allowed('evidence/memory-v1/components/core-native/dev/build.log'))
        for name in ('evidence/memory-v1/author/build.log', 'evidence/other.xml', 'scripts/result.xml', 'evidence/memory-v1/native/binary.exe'):
            self.assertFalse(audit.allowed(name))

    def test_wrong_revision_platform_or_drift(self):
        for key, value in [('platform', 'Darwin'), ('source_drift', True), ('source_after', {}), ('extension', 'original')]:
            data = copy.deepcopy(self.fixture)
            data[key] = value
            with self.assertRaises(ValueError):
                module.validate(data, 'dev', 'fixture-revision')
        with self.assertRaises(ValueError):
            module.validate(self.fixture, 'dev', 'different-revision')

    def test_failed_build_and_empty_test_suite_not_pass(self):
        for step, key, value in [(1, 'status', 'failed'), (1, 'exit_code', 1), (2, 'status', 'not run')]:
            data = copy.deepcopy(self.fixture)
            data['presets']['dev'][step][key] = value
            self.assertEqual(module.validate(data, 'dev', 'fixture-revision'), 'failed')
        self.fixture['presets']['dev'][2]['tests']['count'] = 0
        self.assertEqual(module.validate(self.fixture, 'dev', 'fixture-revision'), 'failed')

    def test_memory_inputs_invalidate_report_identity(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            names = ('CMakeLists.txt', 'memory/CMakeLists.txt', 'report.py',
                     'memory/include/memory_lab/arena.hpp', 'memory/src/main.cpp', 'memory/tests/check_driver.py')
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('fixture input')
            with patch.object(report, 'ROOT', root):
                before = report.source_identity()
                for name in names:
                    path = root / name
                    path.write_text('changed fixture input')
                    self.assertNotEqual(before, report.source_identity(), name)
                    path.write_text('fixture input')
                    self.assertEqual(before, report.source_identity())

    def test_failed_memory_build_marks_dependents_not_run(self):
        with tempfile.TemporaryDirectory() as td:
            inspector = report.Inspector(Path(td))
            inspector.commands = [{'name': 'build-memory', 'status': 'failed', 'exit_code': 1}]
            inspector.errors = ['fixture compiler failure']
            with self.assertRaises(RuntimeError):
                inspector.require_build()
            result = json.loads((Path(td) / 'report.json').read_text())
            self.assertEqual(result['status'], 'failed')
            self.assertEqual(result['dependent_inspection_and_tests'], 'not run')


if __name__ == '__main__':
    unittest.main()
