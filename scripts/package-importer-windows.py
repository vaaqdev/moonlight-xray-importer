"""Finish an x64 Qt-deployed Importer folder with app-local MSVC/UCRT/ICU.

Run from a Visual Studio developer prompt. The input folder must already contain
Moonlight, its Qt/QML deployment, FFmpeg/SDL/OpenSSL DLLs and Xray/xray.exe.
No user profiles, portable settings or logs are ever included in the output ZIP.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

from windows_pe import PE

ICU_VERSION = '72.1.0.3'


def choose(pattern):
    from glob import glob
    matches = sorted(glob(pattern), reverse=True)
    if not matches:
        raise RuntimeError(f'Redistributables not found: {pattern}')
    return Path(matches[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--portable-dir', required=True, type=Path)
    parser.add_argument('--build-exe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--crt-dir', type=Path)
    parser.add_argument('--ucrt-dir', type=Path)
    parser.add_argument('--icu-package', type=Path)
    parser.add_argument('--icu-compat', type=Path, help='Optional previously built ICU compatibility DLL')
    args = parser.parse_args()
    root = args.portable_dir.resolve()
    if not root.is_dir() or not args.build_exe.is_file() or not (root / 'Xray/xray.exe').is_file():
        parser.error('Provide a Qt-deployed portable folder, build executable and bundled Xray')
    pfx86 = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    crt = args.crt_dir or choose(pfx86 + r'\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT')
    if (crt / 'Microsoft.VC143.CRT').is_dir():
        crt = crt / 'Microsoft.VC143.CRT'
    ucrt = args.ucrt_dir or choose(pfx86 + r'\Windows Kits\10\Redist\*\ucrt\DLLs\x64')
    shutil.copy2(args.build_exe, root / 'Moonlight.exe')
    for directory in (crt, crt.parent / 'Microsoft.VC143.OpenMP', crt.parent / 'Microsoft.VC143.CXXAMP', ucrt):
        for dll in directory.glob('*.dll'):
            shutil.copy2(dll, root / dll.name)
    with tempfile.TemporaryDirectory(prefix='moonlight-icu-') as temp:
        work = Path(temp)
        package = args.icu_package or work / 'icu.nupkg'
        if not args.icu_package:
            url = f'https://api.nuget.org/v3-flatcontainer/microsoft.icu.icu4c.runtime.win-x64/{ICU_VERSION}/microsoft.icu.icu4c.runtime.win-x64.{ICU_VERSION}.nupkg'
            urllib.request.urlretrieve(url, package)
        with zipfile.ZipFile(package) as archive:
            for filename in ('icuuc72.dll', 'icuin72.dll', 'icudt72.dll'):
                (root / filename).write_bytes(archive.read('runtimes/win-x64/native/' + filename))
            (root / 'LICENSE-ICU.txt').write_bytes(archive.read('LICENSE'))
        if args.icu_compat:
            shutil.copy2(args.icu_compat, root / 'icuuc.dll')
        else:
            providers = [(name, PE(root / (name + '.dll')).exports()[0]) for name in ('icuuc72', 'icuin72')]
            required = PE(root / 'icuuc.dll').exports()[0] if (root / 'icuuc.dll').exists() else set()
            if not required:
                required = {symbol[:-3] for _, exports in providers for symbol in exports if re.fullmatch(r'[A-Za-z_]\w*_72', symbol)}
            definitions = []
            for name in sorted(required):
                for module, exports in providers:
                    symbol = name if name in exports else name + '_72'
                    if symbol in exports:
                        definitions.append(f'  {name}={module}.{symbol}')
                        break
                else:
                    raise RuntimeError(f'ICU compatibility function missing: {name}')
            definition = work / 'icuuc.def'
            definition.write_text('LIBRARY icuuc\nEXPORTS\n' + '\n'.join(definitions) + '\n', encoding='ascii')
            subprocess.run(['link', '/NOLOGO', '/DLL', '/NOENTRY', '/MACHINE:X64', f'/DEF:{definition}', f'/OUT:{work / "icuuc.dll"}'], cwd=work, check=True)
            shutil.copy2(work / 'icuuc.dll', root / 'icuuc.dll')
    included = [p for p in root.rglob('*') if p.is_file() and p.suffix.lower() not in ('.ini', '.log') and p.name.lower() != 'profile.json']
    dlls = {p.name.lower(): p for p in included if p.suffix.lower() in ('.dll', '.exe')}
    parsed = {name: PE(path) for name, path in dlls.items()}
    system = Path(os.environ['SystemRoot']) / 'System32'
    for name, pe in parsed.items():
        dependencies = pe.imports()
        _, forwarders = pe.exports()
        for target in forwarders.values():
            module, symbol = target.rsplit('.', 1)
            module = module.lower()
            if not module.endswith('.dll'):
                module += '.dll'
            dependencies.add(module)
            if name.startswith('icu'):
                if module not in parsed or symbol not in parsed[module].exports()[0]:
                    raise RuntimeError(f'Invalid ICU forwarder: {name}: {target}')
        if name.startswith('icu') and 'icu.dll' in dependencies:
            raise RuntimeError('Windows ICU dependency must not be included')
        for dependency in dependencies - dlls.keys():
            local_required = dependency.startswith(('icu', 'msvcp', 'vcruntime', 'concrt', 'vcomp', 'vcamp', 'api-ms-win-crt')) or dependency == 'ucrtbase.dll'
            if local_required or not (dependency.startswith(('api-ms-win-', 'ext-ms-win-')) or (system / dependency).is_file()):
                raise RuntimeError(f'Missing dependency: {name}: {dependency}')
    if not args.output.parent.is_dir():
        parser.error('Output parent directory must exist')
    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(included):
            archive.write(path, 'Moonlight-VLESS-Import-Portable/' + path.relative_to(root).as_posix())
    with zipfile.ZipFile(args.output) as archive:
        assert archive.testzip() is None
        assert archive.read('Moonlight-VLESS-Import-Portable/Moonlight.exe') == args.build_exe.read_bytes()
    print(f'PASS: {len(parsed)} x64 binaries audited, including indirect ICU dependencies')
    print(f'ZIP: {args.output}\nSHA256: {hashlib.sha256(args.output.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
