"""Recreate the isolated prototype without changing production sources."""
from pathlib import Path
import shutil
import subprocess
root = Path.cwd()
archive = root / 'tools/experiments/ltm_reference'
work = root / 'tmp/ltm-reference'
dest = work / 'src'
if dest.exists():
    raise SystemExit('tmp/ltm-reference/src already exists; keep it or move it aside before recreating.')
(dest / 'native').mkdir(parents=True)
(dest / 'tools').mkdir()
for parent, copied in [('native', 'local_tone'), ('tools', 'offline_rawr_pipeline')]:
    for source in (root / parent).iterdir():
        target = dest / parent / source.name
        if source.name == copied:
            shutil.copytree(source, target)
        else:
            target.symlink_to(source, target_is_directory=source.is_dir())
for name in ['app', 'cmake']:
    source = root / name
    if source.exists():
        (dest / name).symlink_to(source, target_is_directory=True)
subprocess.run(['git', 'apply', '--directory=tmp/ltm-reference/src', str(archive / 'reference.patch')], check=True)
for source in archive.glob('*.py'):
    if source.name != 'setup.py':
        shutil.copy2(source, work / source.name)
