"""MovMuxer output must decode identically to the source streams and carry timing, rotation and colour."""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile

helper, patch, ffmpeg, ffprobe, build = sys.argv[1:]
def run(*args):
    try:
        return subprocess.check_output([str(x) for x in args], stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as error:
        sys.stderr.buffer.write(error.stderr)
        raise
def probe(file, *args):
    return json.loads(run(ffprobe, '-v', 'error', *args, '-of', 'json', file))
def md5(file, stream):
    return run(ffmpeg, '-v', 'error', '-noautorotate', '-i', file, '-map', f'0:{stream}:0', '-fps_mode', 'passthrough', '-f', 'md5', '-')

def nals(data):
    """Annex B NAL units with their start offsets."""
    starts, i = [], 0
    while True:
        i = data.find(b'\x00\x00\x01', i)
        if i < 0: break
        starts.append(i + 3); i += 3
    for n, start in enumerate(starts):
        end = starts[n + 1] - 3 if n + 1 < len(starts) else len(data)
        while end > start and data[end - 1] == 0: end -= 1
        yield start, data[start:end]

def adts_frames(data):
    i, frames, asc = 0, [], None
    while i + 7 <= len(data):
        assert data[i] == 0xFF and data[i + 1] & 0xF0 == 0xF0
        length = ((data[i + 3] & 3) << 11) | (data[i + 4] << 3) | (data[i + 5] >> 5)
        header = 7 if data[i + 1] & 1 else 9
        if asc is None:
            profile, rate, channels = (data[i + 2] >> 6) + 1, (data[i + 2] >> 2) & 15, ((data[i + 2] & 1) << 2) | (data[i + 3] >> 6)
            asc = struct.pack('>H', (profile << 11) | (rate << 7) | (channels << 3))
        frames.append(data[i + header:i + length]); i += length
    return asc, frames

with tempfile.TemporaryDirectory(prefix="mov-", dir=build) as directory:
    root = Path(directory)
    for name, x265, rotation, fps in (('ip', 'bframes=0', 0, 30), ('ibbp', 'bframes=2:b-adapt=0', 90, 24)):
        source = root/f'{name}-source.mp4'
        run(ffmpeg, '-y', '-v', 'error', '-f', 'lavfi', '-i', f'testsrc2=size=160x96:rate={fps}:duration=2',
            '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=2', '-ac', '2',
            '-pix_fmt', 'yuv420p10le', '-c:v', 'libx265', '-x265-params', f'log-level=error:keyint={fps}:aud=1:{x265}',
            '-c:a', 'aac', '-b:a', '128k', '-shortest', source)
        hevc, adts = root/'v.hevc', root/'a.aac'
        run(ffmpeg, '-y', '-v', 'error', '-i', source, '-map', '0:v', '-c', 'copy', '-bsf:v', 'hevc_mp4toannexb', '-f', 'hevc', hevc)
        run(ffmpeg, '-y', '-v', 'error', '-i', source, '-map', '0:a', '-c', 'copy', '-f', 'adts', adts)
        packets = probe(source, '-select_streams', 'v', '-show_packets', '-show_entries', 'packet=pts_time,dts_time,flags')['packets']
        stream = hevc.read_bytes()
        units, config = [], b''
        for start, nal in nals(stream):
            kind = (nal[0] >> 1) & 63
            if kind == 35:  # AUD starts an access unit (the first one carries none)
                if not units or units[-1]: units.append(b'')
                continue
            if not units: units.append(b'')
            if kind in (32, 33, 34) and len(units) == 1: config += b'\x00\x00\x00\x01' + nal
            units[-1] += b'\x00\x00\x00\x01' + nal
        assert len(units) == len(packets), (len(units), len(packets))
        asc, frames = adts_frames(adts.read_bytes())
        # Offset the tracks like a recording: audio first, video ~0.26 s later.
        # (interleave time, track, sync, pts, data): interleave by decode time, keeping each track's order.
        samples = [(float(p['dts_time']), 0, 'K' in p['flags'], int(float(p['pts_time']) * 1e6) + 260_000, u)
                   for p, u in zip(packets, units)]
        samples += [(i * 1024 / 48000, 1, True, i * 1024 * 1_000_000 // 48000, f) for i, f in enumerate(frames)]
        blob = b''.join(struct.pack('<BBqI', track, sync, pts, len(data)) + data
                        for _, track, sync, pts, data in sorted(samples, key=lambda s: (s[0], s[1])))
        (root/'csd').write_bytes(config); (root/'asc').write_bytes(asc); (root/'samples').write_bytes(blob)
        out = root/f'{name}.mp4'
        run(helper, out, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, rotation, 0)
        info = probe(out, '-show_streams', '-show_format')
        video = next(s for s in info['streams'] if s['codec_type'] == 'video')
        audio = next(s for s in info['streams'] if s['codec_type'] == 'audio')
        assert video['codec_name'] == 'hevc' and video['codec_tag_string'] == 'hvc1', video
        assert video['r_frame_rate'] == f'{fps}/1', video['r_frame_rate']
        assert video.get('color_primaries') == 'bt709' and video.get('color_range') == 'tv', video
        assert int(video['nb_frames']) == len(units)
        assert audio['codec_name'] == 'aac' and audio['channels'] == 2 and audio['sample_rate'] == '48000'
        assert abs(float(video['start_time']) - 0.26) < 0.002, video['start_time']
        assert abs(float(audio['start_time'])) < 0.002, audio['start_time']
        rotations = [d.get('rotation') for d in video.get('side_data_list', []) if 'rotation' in d]
        assert rotations == ([] if rotation == 0 else [-rotation]), rotations
        frames_out = probe(out, '-select_streams', 'v', '-show_frames', '-show_entries', 'frame=pts_time')['frames']
        times = [float(f['pts_time']) for f in frames_out]
        assert times == sorted(times) and abs(times[1] - times[0] - 1 / fps) < 1e-3, times[:4]
        assert md5(out, 'v') == md5(source, 'v'), name
        # The source MP4 trims AAC priming with an edit list; compare against the untrimmed ADTS packets.
        assert md5(out, 'a') == md5(adts, 'a'), name
        run(patch, out)  # Device metadata patcher must accept MovMuxer layout.
        assert probe(out, '-show_format')['format']['tags']['make'] == 'vivo'
        assert md5(out, 'v') == md5(source, 'v'), name
        print(name, 'ok', len(units), 'frames', len(frames), 'AAC packets')

    # PCM in QuickTime: the muxed audio must be the exact input bytes, with the video unchanged.
    for bits in (16, 24):
        raw = root/f'pcm{bits}.raw'
        run(ffmpeg, '-y', '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=48000:duration=2',
            '-ac', '2', '-f', f's{bits}le', raw)
        pcm = raw.read_bytes()
        frame = 2 * bits // 8
        blocks = [pcm[i:i + 1024 * frame] for i in range(0, len(pcm), 1024 * frame)]
        samples = [(float(p['dts_time']), 0, 'K' in p['flags'], int(float(p['pts_time']) * 1e6) + 260_000, u)
                   for p, u in zip(packets, units)]
        samples += [(i * 1024 / 48000, 1, True, i * 1024 * 1_000_000 // 48000, b) for i, b in enumerate(blocks)]
        blob = b''.join(struct.pack('<BBqI', track, sync, pts, len(data)) + data
                        for _, track, sync, pts, data in sorted(samples, key=lambda s: (s[0], s[1])))
        (root/'samples').write_bytes(blob)
        out = root/f'pcm{bits}.mov'
        run(helper, out, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, rotation, bits)
        info = probe(out, '-show_streams', '-show_format')
        audio = next(s for s in info['streams'] if s['codec_type'] == 'audio')
        assert info['format']['format_name'].startswith('mov'), info['format']['format_name']
        assert audio['codec_name'] == f'pcm_s{bits}le' and audio['channels'] == 2 and audio['sample_rate'] == '48000', audio
        assert abs(float(audio['duration']) - len(pcm) / frame / 48000) < 1e-3, audio['duration']
        copied = run(ffmpeg, '-v', 'error', '-i', out, '-map', '0:a', '-c', 'copy', '-f', f's{bits}le', '-')
        assert copied == pcm, f'PCM {bits} bytes differ'
        assert md5(out, 'v') == md5(source, 'v')
        run(patch, out)
        assert run(ffmpeg, '-v', 'error', '-i', out, '-map', '0:a', '-c', 'copy', '-f', f's{bits}le', '-') == pcm
        print(f'pcm{bits} ok', len(blocks), 'blocks')

    # Interrupted recordings: recover from the sample index, for AAC/MP4 and PCM/MOV.
    for bits, crash_at in ((0, 120), (24, 100)):
        out = root/f'crash{bits}.{"mov" if bits else "mp4"}'
        if bits:
            pcm = (root/f'pcm{bits}.raw').read_bytes()
            frame = 2 * bits // 8
            audio_samples = [(i * 1024 / 48000, 1, True, i * 1024 * 1_000_000 // 48000, pcm[i * 1024 * frame:(i + 1) * 1024 * frame])
                             for i in range((len(pcm) + 1024 * frame - 1) // (1024 * frame))]
        else:
            audio_samples = [(i * 1024 / 48000, 1, True, i * 1024 * 1_000_000 // 48000, f) for i, f in enumerate(frames)]
        samples = [(float(p['dts_time']), 0, 'K' in p['flags'], int(float(p['pts_time']) * 1e6) + 260_000, u)
                   for p, u in zip(packets, units)] + audio_samples
        blob = b''.join(struct.pack('<BBqI', track, sync, pts, len(data)) + data
                        for _, track, sync, pts, data in sorted(samples, key=lambda s: (s[0], s[1])))
        (root/'samples').write_bytes(blob)
        run(helper, out, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, rotation, bits, root/'index', crash_at)
        info = probe(out, '-show_streams')
        video = next(s for s in info['streams'] if s['codec_type'] == 'video')
        audio = next(s for s in info['streams'] if s['codec_type'] == 'audio')
        recovered = int(video['nb_frames'])
        assert 0 < recovered < len(units), recovered
        assert audio['codec_name'] == (f'pcm_s{bits}le' if bits else 'aac'), audio['codec_name']
        run(ffmpeg, '-v', 'error', '-xerror', '-i', out, '-f', 'null', '-')
        print(f'crash{bits} ok: recovered {recovered}/{len(units)} frames, audio {float(audio["duration"]):.2f} s')
