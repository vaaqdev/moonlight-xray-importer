"""Windows regression using real Xray behind an eight-second startup delay."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

harness, wrapper, core = map(Path, sys.argv[1:4])
with tempfile.TemporaryDirectory(prefix='slow-xray-fixture-') as temporary:
    root = Path(temporary)
    shutil.copy2(harness, root / harness.name)
    (root / 'Xray').mkdir()
    shutil.copy2(wrapper, root / 'Xray/xray.exe')
    shutil.copy2(core, root / 'Xray/xray-real.exe')
    (root / 'Xray/profile.json').write_text(json.dumps({'outbounds': [{'protocol': 'freedom'}]}))
    env = dict(os.environ, QT_FORCE_STDERR_LOGGING='1')
    for argument in ('--import-tests', '--reconnect-test'):
        result = subprocess.run([str(root / harness.name), argument], capture_output=True, text=True, timeout=90, env=env)
        print(result.stdout, result.stderr, flush=True)
        assert result.returncode == 0, (argument, result.returncode)
        assert 'different thread' not in result.stderr and 'Cannot move' not in result.stderr
    print('PASS: delayed validation and delayed Xray startup/recovery exceed old timeouts and now succeed')
