import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile
import tarfile

spec = importlib.util.spec_from_file_location('assembler', Path(__file__).with_name('assemble-release.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AssemblyTest(unittest.TestCase):
    def fixture(self, root):
        artifacts = root / 'artifacts'
        def artifact(name, file):
            path = artifacts / name / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture binary')
        for platform, arch in [('Linux', 'x86_64'), ('Linux-aarch64', 'aarch64')]:
            artifact(f'PrismLauncher-{platform}-Qt6-Portable-abcdef0-Release', 'PrismLauncher-portable.tar.gz')
            for suffix in ['AppImage', 'AppImage.zsync']:
                artifact(f'PrismLauncher-Linux-abcdef0-Release-{arch}.{suffix}', f'PrismLauncher-Linux-{arch}.{suffix}')
        artifact('PrismLauncher-macOS-abcdef0-Release', 'PrismLauncher.zip')
        artifact('PrismLauncher-macOS-abcdef0-Release.dmg', 'PrismLauncher.dmg')
        for platform in ['Windows-MinGW-w64', 'Windows-MSVC', 'Windows-MSVC-arm64']:
            artifact(f'PrismLauncher-{platform}-Setup-abcdef0-Release', 'PrismLauncher-Setup.exe')
            for kind in ['', '-Portable']:
                artifact(f'PrismLauncher-{platform}{kind}-abcdef0-Release', 'prismlauncher.exe')
                artifact(f'PrismLauncher-{platform}{kind}-abcdef0-Release', 'plugins/platforms/qwindows.dll')
        source = root / 'source'
        (source / '.git').mkdir(parents=True)
        (source / '.git' / 'config').write_text('excluded fixture')
        (source / 'CMakeLists.txt').write_text('source fixture')
        return artifacts, source

    def test_complete_manifest_and_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts, source = self.fixture(root)
            output = root / 'output'
            files = module.assemble(artifacts, source, output, 'v12.0.0-beta.1')
            self.assertEqual(len(files), 18)
            lines = (output / 'SHA256SUMS.txt').read_text().splitlines()
            self.assertEqual(len(lines), 18)
            for line in lines:
                digest, filename = line.split('  ')
                self.assertEqual(digest, hashlib.sha256((output / filename).read_bytes()).hexdigest())
            with zipfile.ZipFile(output / 'PrismLauncher-Windows-MSVC-v12.0.0-beta.1.zip') as archive:
                self.assertIn('plugins/platforms/qwindows.dll', archive.namelist())
            with tarfile.open(output / 'PrismLauncher-v12.0.0-beta.1.tar.gz') as archive:
                self.assertFalse(any('.git' in Path(p).parts for p in archive.getnames()))
                self.assertIn('PrismLauncher-v12.0.0-beta.1/CMakeLists.txt', archive.getnames())

    def test_missing_package_blocks_assembly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts, source = self.fixture(root)
            next(artifacts.glob('**/PrismLauncher.dmg')).unlink()
            with self.assertRaises(ValueError):
                module.assemble(artifacts, source, root / 'output', 'v12.0.0')

    def test_unsafe_version_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(ValueError):
                module.assemble(root, root, root / 'output', '../escape')


if __name__ == '__main__':
    unittest.main()
