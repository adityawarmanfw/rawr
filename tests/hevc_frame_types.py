"""Check slice-header I/P/B classification against ffprobe for real x265 GOPs."""
from pathlib import Path
import subprocess
import sys
import tempfile

helper, ffmpeg, ffprobe, build = sys.argv[1:]
def run(*args):
    return subprocess.check_output([str(x) for x in args], stderr=subprocess.PIPE).decode()

with tempfile.TemporaryDirectory(prefix='gop-', dir=build) as directory:
    for name, options in (('all-intra', 'keyint=1'), ('ip', 'keyint=8:bframes=0'),
                          ('ibbp', 'keyint=12:bframes=2:b-adapt=0'), ('extra-bits', 'keyint=6:bframes=1:b-adapt=0')):
        source = Path(directory)/f'{name}.hevc'
        run(ffmpeg, '-y', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=128x96:rate=10:duration=2',
            '-pix_fmt', 'yuv420p', '-c:v', 'libx265', '-x265-params', f'log-level=error:{options}', '-f', 'hevc', source)
        ours = run(helper, source).strip()
        # ffprobe lists packets in decode order with their picture type via the decoder.
        theirs = ''.join(line.strip().strip(',') for line in run(
            ffprobe, '-v', 'error', '-select_streams', 'v', '-show_entries', 'frame=pict_type',
            '-of', 'csv=p=0', source).splitlines())
        assert sorted(ours) == sorted(theirs), (name, ours, theirs)
        if name == 'all-intra': assert set(ours) == {'I'}, ours
        if name == 'ip': assert 'B' not in ours and 'P' in ours, ours
        if name == 'ibbp': assert 'B' in ours, ours
        print(name, ours)
