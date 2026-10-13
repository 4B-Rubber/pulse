"""Regression checks for the runner; never execute release binaries."""
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import contextlib
import io
import json
import subprocess
import unittest
import verify_win81 as verify


class CurrentBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory(prefix='pulse-verify-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'build-win81').mkdir()
        for name in ('one', 'two'):
            (self.root / 'build-win81' / (name + '.exe')).write_bytes(name.encode())
        self.out = self.root / 'bench_data' / 'win81-tests'
        self.out.mkdir(parents=True)
        self.out.joinpath('results.json').write_text(json.dumps([dict(test='one', exit=0), dict(test='two', exit=0)]))

    def run_fixture(self, runner, selected=()):
        with contextlib.redirect_stdout(io.StringIO()):
            code = verify.run_tests(self.root, selected, runner, ['one', 'two'])
        return code, json.loads(self.out.joinpath('results.json').read_text())

    def test_old_pass_does_not_skip_current_binary(self):
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            self.assertTrue(Path(kwargs['env']['PULSE_TEST_DATA_DIR']).is_relative_to(self.out))
            return SimpleNamespace(returncode=0)
        code, rows = self.run_fixture(runner)
        self.assertEqual((code, len(calls), len(rows)), (0, 2, 2))
        first_run = rows[0]['run']
        code, rows = self.run_fixture(runner)
        self.assertEqual((code, len(calls)), (0, 4))
        self.assertNotEqual(rows[0]['run'], first_run)
        self.assertTrue((self.out / first_run / 'results.json').is_file())
        self.assertEqual(rows[0]['sha256'], verify.fingerprint(self.root / 'build-win81' / 'one.exe'))

    def test_missing_binary_cannot_reuse_prior_pass(self):
        self.root.joinpath('build-win81/one.exe').unlink()
        def runner(*args, **kwargs):
            self.fail('missing executable must not be launched')
        code, rows = self.run_fixture(runner, ['one'])
        self.assertEqual((code, len(rows), rows[0]['exit']), (1, 1, 'launch-error'))

    def test_selection_does_not_claim_unrun_tests(self):
        code, rows = self.run_fixture(lambda *a, **kw: SimpleNamespace(returncode=5), ['two'])
        self.assertEqual((code, [r['test'] for r in rows], rows[0]['exit']), (1, ['two'], 5))

    def test_timeout_is_current_failure(self):
        def runner(args, **kwargs):
            raise subprocess.TimeoutExpired(args, 180)
        code, rows = self.run_fixture(runner, ['one'])
        self.assertEqual((code, rows[0]['exit']), (1, 'timeout'))

    def test_changed_executable_cannot_pass(self):
        def runner(args, **kwargs):
            Path(args[0]).write_bytes(b'replaced-during-run')
            return SimpleNamespace(returncode=0)
        code, rows = self.run_fixture(runner, ['one'])
        self.assertEqual((code, rows[0]['exit']), (1, 'binary-changed'))

    def test_unknown_selection_is_rejected(self):
        with self.assertRaises(ValueError):
            self.run_fixture(lambda *a, **kw: self.fail('must not run'), ['typo'])


if __name__ == '__main__':
    unittest.main()
