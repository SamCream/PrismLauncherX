import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('release-plan.sh').resolve()


class ReleasePlanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.git('init', '-q')
        self.git('-c', 'user.name=CI Fixture', '-c', 'user.email=ci@example.test', 'commit', '-q', '--allow-empty', '-m', 'fixture')
        self.sha = self.git('rev-parse', 'HEAD').strip()
        self.git('tag', 'v1.0.0')
        self.git('tag', 'v1.0.0-beta.1')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True)

    def plan(self, event='push', ref_type='branch', ref_name='feature', publish='false', tag='', prerelease='false', sha=None):
        output = self.root / 'outputs'
        output.write_text('')
        env = {**os.environ, 'GITHUB_EVENT_NAME': event, 'GITHUB_REF_TYPE': ref_type, 'GITHUB_REF_NAME': ref_name,
               'GITHUB_SHA': sha or self.sha, 'REQUEST_PUBLISH': publish, 'REQUEST_TAG': tag,
               'REQUEST_PRERELEASE': prerelease, 'GITHUB_OUTPUT': str(output)}
        result = subprocess.run(['bash', str(SCRIPT)], cwd=self.root, env=env, capture_output=True, text=True)
        values = dict(line.split('=', 1) for line in output.read_text().splitlines())
        return result, values

    def test_branch_push_builds_only(self):
        result, values = self.plan()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values['publish'], 'false')
        self.assertTrue(values['version'].startswith('snapshot-'))

    def test_tag_push_publishes(self):
        result, values = self.plan(ref_type='tag', ref_name='v1.0.0')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values['publish'], 'true')
        self.assertEqual(values['prerelease'], 'false')

    def test_prerelease_tag(self):
        result, values = self.plan(ref_type='tag', ref_name='v1.0.0-beta.1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values['prerelease'], 'true')

    def test_manual_tag_ref_does_not_publish_implicitly(self):
        result, values = self.plan(event='workflow_dispatch', ref_type='tag', ref_name='v1.0.0')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values['publish'], 'false')

    def test_explicit_manual_publication(self):
        result, values = self.plan(event='workflow_dispatch', publish='true', tag='v1.0.0')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values['tag'], 'v1.0.0')
        self.assertEqual(values['publish'], 'true')

    def test_invalid_and_missing_tag(self):
        for tag in ['', '../unsafe', 'not-a-version']:
            result, _ = self.plan(event='workflow_dispatch', publish='true', tag=tag)
            self.assertNotEqual(result.returncode, 0)

    def test_mismatched_commit(self):
        result, _ = self.plan(ref_type='tag', ref_name='v1.0.0', sha='0' * 40)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
