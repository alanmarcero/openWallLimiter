"""Exercise publishing without writing to GitHub."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import publish_release


class PublishReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.real_run = subprocess.run
        self.real_run(['git', 'init', '-q', str(self.source)], check=True)
        self.real_run(['git', '-C', str(self.source), '-c', 'user.name=Test', '-c',
                       'user.email=test@example.com', 'commit', '-qm', 'source', '--allow-empty'], check=True)
        self.real_run(['git', '-C', str(self.source), 'tag', 'v1.0.0'], check=True)
        self.revision = subprocess.check_output(['git', '-C', str(self.source), 'rev-parse', 'HEAD'], text=True).strip()
        (self.source / 'docs').mkdir()
        (self.source / 'docs/RELEASE_NOTES.md').write_text('Release notes')
        self.dist = self.root / 'dist'
        self.remote = self.root / 'remote'
        self.dist.mkdir()
        self.remote.mkdir()
        self.package_names = ['openWallLimiter-macOS-universal.zip', 'openWallLimiter-Windows-x64.zip']
        self.make_packages(self.dist, 'new build')
        self.make_packages(self.remote, 'original build')
        self.metadata = {
            'tag_name': 'v1.0.0', 'draft': False,
            'target_commitish': self.revision, 'published_at': '2026-10-21T15:26:33Z',
            'assets': [{'name': name, 'state': 'uploaded'} for name in self.package_names + ['SHA256SUMS.txt']],
        }
        self.api_status = 200
        self.create_status = 0
        self.download_status = 0
        self.created = self.root / 'created'
        self.before = {p.name: p.read_bytes() for p in self.remote.iterdir()}
        self.patcher = patch.object(subprocess, 'run', side_effect=self.run_command)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)

    def make_packages(self, folder, payload, revision=None):
        for name in self.package_names:
            with zipfile.ZipFile(folder / name, 'w') as archive:
                archive.writestr('package/SOURCE.txt', 'Plugin source and build instructions: '
                                 f'https://github.com/test/openWallLimiter/tree/{revision or self.revision}\n')
                archive.writestr('package/plugin.bin', payload)
        self.write_checksums(folder)

    def write_checksums(self, folder):
        (folder / 'SHA256SUMS.txt').write_text(''.join(
            f'{hashlib.sha256((folder / name).read_bytes()).hexdigest()}  {name}\n'
            for name in self.package_names))

    def run_command(self, command, **kwargs):
        if command[0] != 'gh':
            return self.real_run(command, **kwargs)
        if command[1] == 'api':
            self.assertEqual(command[2], 'repos/test/openWallLimiter/releases/tags/v1.0.0')
            body = self.metadata if self.api_status == 200 else {'status': str(self.api_status), 'message': 'API failure'}
            result = subprocess.CompletedProcess(command, int(self.api_status != 200), json.dumps(body), 'API failure')
        elif command[1:3] == ['release', 'download']:
            self.assertEqual(command[3:6], ['v1.0.0', '--repo', 'test/openWallLimiter'])
            destination = Path(command[command.index('--dir') + 1])
            for index, value in enumerate(command):
                if value == '--pattern':
                    shutil.copy2(self.remote / command[index + 1], destination)
            result = subprocess.CompletedProcess(command, self.download_status)
        elif command[1:3] == ['release', 'create']:
            self.assertEqual(command[3], 'v1.0.0')
            self.assertIn('--verify-tag', command)
            self.assertEqual(command[command.index('--target') + 1], self.revision)
            self.assertEqual(Path(command[command.index('--notes-file') + 1]).read_text(), 'Release notes')
            if self.api_status == 404 and self.create_status == 0:
                self.created.mkdir()
                for path in command[4:]:
                    if Path(path).is_file() and Path(path).parent == self.dist:
                        shutil.copy2(path, self.created)
            result = subprocess.CompletedProcess(command, self.create_status or int(self.api_status != 404))
        else:
            self.fail(f'Unexpected GitHub mutation: {command}')
        if kwargs.get('check') and result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command)
        return result

    def publish(self):
        publish_release.publish('v1.0.0', 'test/openWallLimiter', self.source, self.dist)

    def assert_rejected_without_mutation(self):
        with self.assertRaises((ValueError, RuntimeError, subprocess.CalledProcessError, OSError)):
            self.publish()
        self.assertFalse(self.created.exists())
        self.assertEqual({p.name: p.read_bytes() for p in self.remote.iterdir()}, self.before)

    def test_existing_valid_release_uses_original_checksums_and_preserves_bytes(self):
        self.publish()
        self.assertFalse(self.created.exists())
        self.assertEqual({p.name: p.read_bytes() for p in self.remote.iterdir()}, self.before)

    def test_absent_release_creates_all_assets_for_verified_tag(self):
        self.api_status = 404
        self.publish()
        self.assertEqual({p.name: p.read_bytes() for p in self.created.iterdir()},
                         {p.name: p.read_bytes() for p in self.dist.iterdir()})

    def test_api_failure_is_not_treated_as_absent_release(self):
        for status in (401, 403, 500):
            with self.subTest(status=status):
                self.api_status = status
                self.assert_rejected_without_mutation()

    def test_missing_published_asset_fails(self):
        self.metadata['assets'].pop()
        self.assert_rejected_without_mutation()

    def test_draft_or_wrong_tag_fails(self):
        for field, value in [('draft', True), ('tag_name', 'v9.0.0')]:
            with self.subTest(field=field):
                original = self.metadata[field]
                self.metadata[field] = value
                self.assert_rejected_without_mutation()
                self.metadata[field] = original

    def test_wrong_checksum_fails(self):
        (self.remote / self.package_names[0]).write_bytes(b'corrupt')
        self.before = {p.name: p.read_bytes() for p in self.remote.iterdir()}
        self.assert_rejected_without_mutation()

    def test_manifest_must_cover_each_expected_package_once(self):
        manifest = self.remote / 'SHA256SUMS.txt'
        original = manifest.read_text()
        for text in (original.splitlines()[0] + '\n', original + original, 'bad checksum\n'):
            with self.subTest(manifest=text):
                manifest.write_text(text)
                self.before = {p.name: p.read_bytes() for p in self.remote.iterdir()}
                self.assert_rejected_without_mutation()

    def test_matching_checksum_with_wrong_source_still_fails(self):
        self.make_packages(self.remote, 'wrong source', revision='0' * 40)
        self.before = {p.name: p.read_bytes() for p in self.remote.iterdir()}
        self.assert_rejected_without_mutation()

    def test_checkout_must_match_requested_tag(self):
        self.real_run(['git', '-C', str(self.source), '-c', 'user.name=Test', '-c',
                       'user.email=test@example.com', 'commit', '-qm', 'different source', '--allow-empty'], check=True)
        self.assert_rejected_without_mutation()

    def test_invalid_local_packages_are_not_published(self):
        self.api_status = 404
        (self.dist / self.package_names[0]).unlink()
        self.assert_rejected_without_mutation()

    def test_download_and_create_failures_propagate(self):
        self.download_status = 1
        self.assert_rejected_without_mutation()
        self.api_status = 404
        self.create_status = 1
        self.assert_rejected_without_mutation()


if __name__ == '__main__':
    unittest.main()
