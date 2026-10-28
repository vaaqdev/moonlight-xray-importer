"""Rewrite source-built libraries to self-contained @rpath install names."""
from pathlib import Path
import subprocess
import sys

directory = Path(sys.argv[1]).resolve()
for library in directory.glob('*.dylib'):
    if library.is_symlink():
        continue
    subprocess.run(['install_name_tool', '-id', '@rpath/' + library.name, str(library)], check=True)
    linked = subprocess.check_output(['otool', '-L', str(library)], text=True)
    for line in linked.splitlines():
        if not line.startswith('\t'):
            continue
        dependency = line.strip().split(' (', 1)[0]
        if dependency.startswith(str(directory) + '/'):
            subprocess.run(['install_name_tool', '-change', dependency,
                            '@rpath/' + Path(dependency).name, str(library)], check=True)
