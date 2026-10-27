import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import capabilities
import investigate_server
import public_audit
import verify


class EvidenceTests(unittest.TestCase):
    def test_junit_actual_counts(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'result.xml'
            p.write_text('<testsuite><testcase name="ok" time=".1"/><testcase name="bad"><failure/></testcase><testcase name="skip"><skipped/></testcase></testsuite>')
            result = verify.test_results(p)
            self.assertEqual(result['count'], 3)
            self.assertEqual(result['passed'], 1)
            self.assertEqual(result['status'], 'failed')
            p.write_text('<testsuite/>')
            self.assertEqual(verify.test_results(p)['status'], 'failed')

    def test_public_allowlist(self):
        self.assertTrue(public_audit.allowed('projects/tcp-kv/src/server.cpp'))
        for path in ['learn.py', 'author/solution.cpp', '.env', 'projects/other/a.cpp', '../secret.json']:
            self.assertFalse(public_audit.allowed(path))

    def test_percentile_boundaries(self):
        self.assertEqual(investigate_server.percentile([9, 1, 5], 0), 1)
        self.assertEqual(investigate_server.percentile([9, 1, 5], 1), 9)

    def test_command_failure_is_not_a_pass(self):
        result = verify.command([sys.executable, '-c', 'raise SystemExit(7)'])
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['exit_code'], 7)

    def test_command_deadline_is_failure(self):
        result = verify.command([sys.executable, '-c', 'import time; time.sleep(5)'], timeout=.05)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['reason'], 'deadline')
        self.assertLess(result['seconds'], 3)

    def test_unavailable_tool(self):
        result = capabilities.probe(['cpp-systems-deliberately-nonexistent-command'])
        self.assertEqual(result['status'], 'unavailable')

    def test_source_identity_is_stable_and_excludes_outputs(self):
        a = verify.identity()
        b = verify.identity()
        self.assertEqual(a, b)
        self.assertEqual(len(a['sha256']), 64)
        self.assertTrue(a['files'])
        for p in a['files']:
            self.assertFalse(any(x in {'build', 'reports', 'evidence', '.git', '__pycache__'} for x in Path(p).parts))


if __name__ == '__main__':
    unittest.main()
