"""Verify both release ZIPs, their full checksums, runtime files and licenses."""
import argparse
import hashlib
import json
import re
import struct
import zipfile
from pathlib import Path, PurePosixPath


def verify(archive, required):
    with zipfile.ZipFile(archive) as package:
        files = {}
        for entry in package.infolist():
            if entry.is_dir():
                continue
            name = entry.filename.replace('\\', '/')
            path = PurePosixPath(name)
            assert not path.is_absolute() and '..' not in path.parts, name
            assert name not in files, f'duplicate ZIP entry: {name}'
            files[name] = package.read(entry)
        assert set(files) == required | {'SHA256SUMS.txt'}, (set(files) - required, required - set(files))
        declared = {}
        for line in files['SHA256SUMS.txt'].decode('ascii').splitlines():
            digest, name = line.split('  ', 1)
            assert re.fullmatch('[0-9A-Fa-f]{64}', digest) and name not in declared
            declared[name] = digest.upper()
        assert set(declared) == set(files) - {'SHA256SUMS.txt'}
        for name, digest in declared.items():
            assert hashlib.sha256(files[name]).hexdigest().upper() == digest, name
        for name, data in files.items():
            if name.endswith(('.exe', '.dll')):
                assert data[:2] == b'MZ', name
                pe = struct.unpack_from('<I', data, 0x3c)[0]
                assert data[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', data, pe+4)[0] == 0x8664, name
        if 'RgbmBindings~/dependency-manifest.json' in files:
            manifest = json.loads(files['RgbmBindings~/dependency-manifest.json'].decode('utf-8-sig'))
            assert manifest['package_kind'] == 'rgbm_bindings_runtime_dependencies'
            assert 'source_dia' not in manifest
            for record in manifest['files']:
                assert declared['RgbmBindings~/' + record['Name']] == record['SHA256'].upper()
        return dict(archive=archive.name, size=archive.stat().st_size,
                    sha256=hashlib.sha256(archive.read_bytes()).hexdigest().upper(),
                    files=declared, passed=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', required=True)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    assert re.fullmatch(r'v\d+\.\d+\.\d+', args.version)
    shell = {'compress_bc7e.dll', 'exports.txt', 'enable_unity_rdo.ps1', 'unity_dirs.ps1',
             'bc7_rdo_config.txt.example', 'README-release.txt', 'Licenses~/BC7ENC-RDO-LICENSE.txt'}
    plugin = {'hook_loader.cs', 'Bc7Rdo.Editor.asmdef', 'bc7_rdo_hook.dll',
              'bc7_rgbm_coordinator.dll', 'bc7_rgbm_loader_gate.dll', 'bc7_rdo_config.txt',
              'README-editor-plugin.txt', 'Licenses~/BC7ENC-RDO-LICENSE.txt', 'Licenses~/DETOURS-LICENSE.md'}
    plugin.update('RgbmBindings~/' + name for name in (
        'bc7_rgbm_resolver.exe', 'bc7_rgbm_bindings.exe', 'msdia140.dll',
        'DETOURS-LICENSE.md', 'DEPENDENCY-NOTICE.txt', 'dependency-manifest.json'))
    reports = [verify(args.root / 'release' / f'compress_bc7e_rdo-{args.version}-win-x64.zip', shell),
               verify(args.root / 'release' / f'compress_bc7e_rdo-editor-plugin-{args.version}-win-x64.zip', plugin)]
    summary = dict(version=args.version, passed=True, packages=reports)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({**summary, 'packages': [{k: v for k, v in item.items() if k != 'files'} for item in reports]}))


if __name__ == '__main__':
    main()
