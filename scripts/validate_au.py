"""Validate a signed AU in an isolated temporary installation; leave existing files alone."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

source = Path(sys.argv[1]).resolve()
destination = Path.home() / 'Library/Audio/Plug-Ins/Components/openWallLimiter.component'
if destination.exists() or destination.is_symlink():
    raise SystemExit('Validation refuses to replace an existing installed component')
destination.parent.mkdir(parents=True, exist_ok=True)
shutil.copytree(source, destination)
try:
    subprocess.run(['killall', 'AudioComponentRegistrar'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    subprocess.run(['auval', '-a'], check=True)
    subprocess.run(['auval', '-v', 'aufx', 'Owli', 'Amrc'], check=True)
finally:
    shutil.rmtree(destination)
