"""MovMuxer output must decode identically to the source streams and carry timing, rotation and colour."""
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

helper, patch, ffmpeg, ffprobe, build = sys.argv[1:]
def run(*args, **environment):
    try:
        return subprocess.check_output([str(x) for x in args], stderr=subprocess.PIPE, env=os.environ | environment)
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

def boxes(data, start=0, end=None):
    end = len(data) if end is None else end
    while start < end:
        size, kind = struct.unpack_from('>I4s', data, start)
        header = 8
        if size == 1:
            size, = struct.unpack_from('>Q', data, start + 8)
            header = 16
        assert size >= header and start + size <= end, (kind, size)
        yield kind, data[start + header:start + size]
        start += size

def gps_metadata(file):
    movie = next(body for kind, body in boxes(file.read_bytes()) if kind == b'moov')
    user, keyed, itemlist = [], {}, []
    for kind, body in boxes(movie):
        if kind == b'udta':
            for tag, value in boxes(body):
                assert tag in (b'\xa9xyz', b'\xa9mak', b'\xa9mod', b'\xa9too', b'meta')
                if tag == b'\xa9xyz':
                    length, language = struct.unpack_from('>HH', value)
                    assert language == 0x15c7 and length == len(value) - 4
                    user.append(value[4:].decode('ascii'))
                elif tag == b'meta':
                    assert value[:4] == bytes(4)
                    children = list(boxes(value, 4))
                    assert [child for child, _ in children] == [b'hdlr', b'ilst']
                    assert children[0][1] == bytes(8) + b'mdirappl' + bytes(9)
                    items = list(boxes(children[1][1]))
                    assert len(items) == 1 and items[0][0] == b'\xa9xyz'
                    values = list(boxes(items[0][1]))
                    assert len(values) == 1 and values[0][0] == b'data'
                    assert values[0][1][:8] == struct.pack('>II', 1, 0)
                    itemlist.append(values[0][1][8:].decode('ascii'))
        if kind == b'meta':
            children = dict(boxes(body))
            assert children[b'hdlr'][8:12] == b'mdta'
            count, = struct.unpack_from('>I', children[b'keys'], 4)
            keys = [value.decode('ascii') for _, value in boxes(children[b'keys'], 8)]
            assert len(keys) == count
            for index, item in boxes(children[b'ilst']):
                values = list(boxes(item))
                assert len(values) == 1 and values[0][0] == b'data'
                value = values[0][1]
                assert value[:8] == struct.pack('>II', 1, 0)
                key = keys[int.from_bytes(index, 'big') - 1]
                assert key not in keyed
                keyed[key] = value[8:].decode('ascii')
    return user, keyed, itemlist

def check_gps(file, horizontal='+12.34568-098.76544', optionals=True, accuracy='0.00', height='0'):
    user, keyed, itemlist = gps_metadata(file)
    assert user == [horizontal + '/'], user
    assert keyed['com.apple.quicktime.location.ISO6709'] == user[0], keyed
    expected_itemlist = [] if height == 'absent' else [
        horizontal + f'{float(height):+.2f}' + 'CRS<http://www.opengis.net/def/crs/EPSG/0/4979>/']
    assert itemlist == expected_itemlist, itemlist
    expected_location_keys = {'com.apple.quicktime.location.ISO6709'}
    if optionals:
        assert keyed['com.apple.quicktime.location.date'] == '2023-11-14T22:13:20.123Z'
        expected_location_keys.add('com.apple.quicktime.location.date')
        if accuracy is None:
            assert 'com.apple.quicktime.location.accuracy.horizontal' not in keyed
        else:
            assert keyed['com.apple.quicktime.location.accuracy.horizontal'] == accuracy
            expected_location_keys.add('com.apple.quicktime.location.accuracy.horizontal')
    else:
        assert 'com.apple.quicktime.location.date' not in keyed
        assert 'com.apple.quicktime.location.accuracy.horizontal' not in keyed
    assert {key for key in keyed if any(part in key.lower() for part in ('location', 'altitude', 'height'))} == expected_location_keys
    assert all('CRS<' not in value for value in keyed.values())

def check_index_gps(file, height, latitude=12.345675, longitude=-98.765435, accuracy=0.0, fix_time=1_700_000_000_123):
    data = file.read_bytes()
    assert data[:4] == b'RMX2'
    records, offset = [], 4
    while offset < len(data):
        assert offset + 5 <= len(data)
        kind, length = struct.unpack_from('<BI', data, offset)
        offset += 5
        assert offset + length <= len(data)
        records.append((kind, data[offset:offset + length]))
        offset += length
    present_height = height != 'absent'
    flags = int(present_height) | (2 if accuracy is not None else 0)
    expected = struct.pack('<Bddddq', flags, latitude, longitude,
                           float(height) if present_height else 0.0,
                           accuracy if accuracy is not None else 0.0, fix_time)
    assert records[0][0] == 0 and records[1] == (5, expected)
    assert sum(kind == 5 for kind, _ in records) == 1
    return data

def media_signature(file):
    top = dict(boxes(file.read_bytes()))
    tracks = [body for kind, body in boxes(top[b'moov']) if kind == b'trak']
    return top[b'ftyp'], top[b'mdat'], tracks

with tempfile.TemporaryDirectory(prefix="mov-", dir=build) as directory:
    root = Path(directory)
    def mux_args(file, bits, index=None, crash_at=0):
        args = [helper, file, root/'csd', root/'asc', root/'samples', 160, 96, fps, 48000, 2, rotation, bits]
        return [str(value) for value in args + ([index, crash_at] if index is not None else [])]

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
        run(*mux_args(out, 0))
        assert gps_metadata(out) == ([], {}, [])
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
        run(*mux_args(out, bits))
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
        run(*mux_args(out, bits, root/'index', crash_at))
        assert (root/'index').read_bytes()[:4] == b'RMX1'
        assert gps_metadata(out) == ([], {}, [])
        info = probe(out, '-show_streams')
        video = next(s for s in info['streams'] if s['codec_type'] == 'video')
        audio = next(s for s in info['streams'] if s['codec_type'] == 'audio')
        recovered = int(video['nb_frames'])
        assert 0 < recovered < len(units), recovered
        assert audio['codec_name'] == (f'pcm_s{bits}le' if bits else 'aac'), audio['codec_name']
        run(ffmpeg, '-v', 'error', '-xerror', '-i', out, '-f', 'null', '-')
        print(f'crash{bits} ok: recovered {recovered}/{len(units)} frames, audio {float(audio["duration"]):.2f} s')

    for bits in (0, 16, 24):
        if bits:
            pcm = (root/f'pcm{bits}.raw').read_bytes()
            frame = 2 * bits // 8
            audio_samples = [(i / frame / 48000, 1, True, i * 1_000_000 // frame // 48000, pcm[i:i + 1024 * frame])
                             for i in range(0, len(pcm), 1024 * frame)]
        else:
            audio_samples = [(i * 1024 / 48000, 1, True, i * 1024 * 1_000_000 // 48000, value)
                             for i, value in enumerate(frames)]
        samples = [(float(packet['dts_time']), 0, 'K' in packet['flags'], int(float(packet['pts_time']) * 1e6) + 260_000, unit)
                   for packet, unit in zip(packets, units)] + audio_samples
        (root/'samples').write_bytes(b''.join(struct.pack('<BBqI', track, sync, pts, len(value)) + value
                                            for _, track, sync, pts, value in sorted(samples, key=lambda sample: (sample[0], sample[1]))))
        references = {}
        for crash_at in (0, 100):
            reference = root/f'reference-{bits}-{crash_at}'
            run(*mux_args(reference, bits, root/'index', crash_at))
            references[crash_at] = media_signature(reference)
        for height in ('absent', '0', '-31.25', '123.50'):
            for crash_at in (0, 100):
                out = root/f'gps-{bits}-{height}-{crash_at}.{"mov" if bits else "mp4"}'
                run(*mux_args(out, bits, root/'index', crash_at), RAWR_TEST_GPS=height)
                check_gps(out, height=height)
                assert media_signature(out) == references[crash_at], 'GPS changed samples, offsets, timing, rotation or color'
                full_index = check_index_gps(root/'index', height)
                run(helper, '--recover', out, root/'index')
                check_gps(out, height=height)
                assert media_signature(out) == references[crash_at]
                assert (root/'index').read_bytes() == full_index, 'Recovery changed the durable GPS input'
                for _ in range(2):
                    run(patch, out)
                    check_gps(out, height=height)
                    assert media_signature(out) == references[crash_at]
                run(patch, out, '--log')
                check_gps(out, height=height)
                assert media_signature(out)[:2] == references[crash_at][:2], 'LOG patch changed sample bytes'
                run(ffmpeg, '-v', 'error', '-xerror', '-i', out, '-f', 'null', '-')
                if exiftool := shutil.which('exiftool'):
                    decoded = json.loads(run(exiftool, '-j', '-n', '-a', '-G1', '-GPSCoordinates', '-Warning', '-Error', out))[0]
                    assert 'UserData:GPSCoordinates' in decoded and 'Keys:GPSCoordinates' in decoded
                    for group in ('UserData', 'Keys'):
                        assert [float(value) for value in decoded[f'{group}:GPSCoordinates'].split()] == [12.34568, -98.76544]
                    if height == 'absent':
                        assert 'ItemList:GPSCoordinates' not in decoded
                    else:
                        assert [float(value) for value in decoded['ItemList:GPSCoordinates'].split()] == [12.34568, -98.76544, float(height)]
                    assert not any(key.rsplit(':', 1)[-1] in ('Warning', 'Error') for key in decoded)
        print(f'gps{bits} 2D primary tags, qualified ItemList height, full RMX2, repeated patch and recovery ok')
        for uncertainty in ('50.01', '123.45', '90000000000000', '90000000000001', '-1', 'nan', 'inf'):
            expected_accuracy = f'{float(uncertainty):.2f}' if uncertainty in ('50.01', '123.45', '90000000000000') else None
            for crash_at in (0, 100):
                policy_out = root/f'uncertainty-{bits}-{uncertainty}-{crash_at}'
                run(*mux_args(policy_out, bits, root/'policy-index', crash_at),
                    RAWR_TEST_GPS='0', RAWR_TEST_ACCURACY=uncertainty)
                check_gps(policy_out, accuracy=expected_accuracy)
                check_index_gps(root/'policy-index', '0', accuracy=float(uncertainty) if expected_accuracy is not None else None)
                run(patch, policy_out, '--log')
                check_gps(policy_out, accuracy=expected_accuracy)
        print(f'gps{bits} uncertainty serialization independent of admission cutoff ok')

    original = out.read_bytes()
    index_bytes = (root/'index').read_bytes()
    header_end = 4 + 5 + struct.unpack_from('<I', index_bytes, 5)[0]
    location_end = header_end + 5 + struct.unpack_from('<I', index_bytes, header_end + 1)[0]
    location_record = index_bytes[header_end:location_end]
    malformed = {
        'unknown-version': b'RMX9' + index_bytes[4:],
        'missing-location': index_bytes[:header_end] + index_bytes[location_end:],
        'duplicate-location': index_bytes[:location_end] + location_record + index_bytes[location_end:],
        'truncated-location': index_bytes[:location_end - 1],
        'legacy-with-location': b'RMX1' + index_bytes[4:],
        'late-location': index_bytes[:header_end] + index_bytes[location_end:] + location_record,
    }
    for name, damaged in malformed.items():
        out.write_bytes(original)
        (root/'bad-index').write_bytes(damaged)
        result = subprocess.run([helper, '--recover', str(out), str(root/'bad-index')], capture_output=True)
        assert result.returncode != 0, name
        assert out.read_bytes() == original, f'{name}: failed recovery changed movie'
        assert (root/'bad-index').read_bytes() == damaged
    out.write_bytes(original)
    (root/'bad-index').write_bytes(index_bytes + b'\x03\x10')
    run(helper, '--recover', out, root/'bad-index')
    check_gps(out, height='123.50')
    print('RMX2 malformed/missing/duplicate/truncated/version rejection and partial sample-tail recovery ok')

    for point, horizontal in (('zero', '+00.00000+000.00000'), ('limits', '-90.00000+180.00000')):
        for height in ('absent', 'nan', 'inf', '1e100'):
            for optionals in ('missing', 'invalid'):
                run(*mux_args(out, 24, root/'index'), RAWR_TEST_GPS=height, RAWR_TEST_POINT=point,
                    RAWR_TEST_OPTIONALS=optionals, LC_ALL='de_DE.UTF-8')
                check_gps(out, horizontal, optionals=False, height='absent')
                latitude, longitude = (-0.000004, 0.0) if point == 'zero' else (-90.0, 180.0)
                check_index_gps(root/'index', 'absent', latitude, longitude, accuracy=None, fix_time=0)
    run(*mux_args(out, 24, root/'index'), RAWR_TEST_GPS='12345.67', LC_ALL='de_DE.UTF-8')
    check_gps(out, height='12345.67')
    check_index_gps(root/'index', '12345.67')
    result = subprocess.run(mux_args(out, 24), capture_output=True,
                            env=os.environ | {'RAWR_TEST_GPS': '0', 'RAWR_TEST_POINT': 'invalid'})
    assert result.returncode != 0 and b'invalid movie location' in result.stderr
    run(helper, '--check-descriptors')
    print('zero/range/locale/invalid optional fields and unseekable output checks ok')
