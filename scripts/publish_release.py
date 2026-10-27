"""Create a release, or verify an already-published release without replacing it."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import quote
import zipfile

PACKAGES = ('openWallLimiter-macOS-universal.zip', 'openWallLimiter-Windows-x64.zip')
ASSETS = (*PACKAGES, 'SHA256SUMS.txt')


def validate_packages(directory, repository, revision):
    checksums = {}
    for line in (directory / 'SHA256SUMS.txt').read_text().splitlines():
        match = re.fullmatch(r'([a-fA-F0-9]{64}) [ *](.+)', line)
        if not match or match[2] not in PACKAGES or match[2] in checksums:
            raise ValueError('Invalid or duplicate entry in SHA256SUMS.txt')
        checksums[match[2]] = match[1].lower()
    if set(checksums) != set(PACKAGES):
        raise ValueError('SHA256SUMS.txt must cover both release packages')
    source = f'Plugin source and build instructions: https://github.com/{repository}/tree/{revision}'
    for name in PACKAGES:
        path = directory / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != checksums[name]:
            raise ValueError(f'Checksum mismatch: {name}')
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                raise ValueError(f'Corrupt ZIP: {name}')
            if archive.read('package/SOURCE.txt').decode().splitlines()[0] != source:
                raise ValueError(f'Package source does not match the release tag: {name}')


def publish(tag, repository, source_directory, dist):
    if not tag.startswith('v'):
        raise ValueError('Release tag must start with v')
    revision = subprocess.check_output(
        ['git', '-C', str(source_directory), 'rev-parse', '--verify', f'refs/tags/{tag}^{{commit}}'],
        text=True).strip()
    checked_out = subprocess.check_output(
        ['git', '-C', str(source_directory), 'rev-parse', 'HEAD'], text=True).strip()
    if checked_out != revision:
        raise ValueError('Checked-out source does not match the release tag')

    result = subprocess.run(
        ['gh', 'api', f'repos/{repository}/releases/tags/{quote(tag, safe="")}'],
        capture_output=True, text=True)
    release = json.loads(result.stdout)
    if result.returncode:
        if str(release.get('status')) != '404':
            raise RuntimeError(f'Release lookup failed: {result.stderr.strip()}')
        validate_packages(dist, repository, revision)
        subprocess.run([
            'gh', 'release', 'create', tag, *(str(dist / name) for name in ASSETS),
            '--repo', repository, '--verify-tag', '--target', revision,
            '--notes-file', str(source_directory / 'docs/RELEASE_NOTES.md'),
            '--title', f'openWallLimiter {tag}',
        ], check=True)
        return

    if release['tag_name'] != tag or release['draft'] or not release['published_at']:
        raise ValueError('Existing release must be published for the requested tag')
    uploaded = {asset['name'] for asset in release['assets'] if asset['state'] == 'uploaded'}
    if not set(ASSETS).issubset(uploaded):
        raise ValueError('Existing release is incomplete; refusing to replace published assets')
    with tempfile.TemporaryDirectory() as temporary:
        command = ['gh', 'release', 'download', tag, '--repo', repository, '--dir', temporary]
        for name in ASSETS:
            command.extend(['--pattern', name])
        subprocess.run(command, check=True)
        # Rebuilt ZIPs can differ; the published manifest authenticates the published bytes.
        validate_packages(Path(temporary), repository, revision)
    print(f'Existing release {tag} has valid packages for {revision}; preserved all assets.')


if __name__ == '__main__':
    publish(os.environ['RELEASE_TAG'], os.environ['GITHUB_REPOSITORY'],
            Path('release-source'), Path('dist'))
