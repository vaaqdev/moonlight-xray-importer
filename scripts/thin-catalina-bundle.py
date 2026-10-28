"""Keep only the Intel slice, before signing the Catalina bundle."""
from pathlib import Path
import subprocess
import sys

app = Path(sys.argv[1]).resolve()
magic = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe',
         b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf'}
for binary in app.rglob('*'):
    if not binary.is_file() or binary.is_symlink():
        continue
    with binary.open('rb') as stream:
        if stream.read(4) not in magic:
            continue
    architectures = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).split()
    if 'x86_64' not in architectures:
        raise RuntimeError(f'No Intel slice: {binary}: {architectures}')
    if len(architectures) > 1:
        output = binary.with_name(binary.name + '.intel-tmp')
        subprocess.run(['lipo', str(binary), '-thin', 'x86_64', '-output', str(output)], check=True)
        output.chmod(binary.stat().st_mode)
        output.replace(binary)
        print('Kept Intel slice:', binary.relative_to(app))
