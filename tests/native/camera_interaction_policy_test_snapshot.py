import json, subprocess, sys
snapshot = json.loads(subprocess.check_output([sys.argv[1], '--snapshot']))
assert snapshot['tapAfActive'] is True
assert snapshot['focusRequestId'] == 42
assert snapshot['spotAeActive'] is True
assert snapshot['videoMode'] is True and snapshot['videoPreviewFps'] == 24
assert snapshot['requestedShutterAngleDegrees'] == 270
assert snapshot['requestedExposureTimeNs'] == 31250000
assert snapshot['shutterAngleChoices'] == [
    {'degrees': (100000 * 24 / 1000000000) * 360, 'exposureTimeNs': 100000},
    {'degrees': 1.40625, 'exposureTimeNs': 162760},
    {'degrees': 2.8125, 'exposureTimeNs': 325520},
    {'degrees': 5.625, 'exposureTimeNs': 651041},
    {'degrees': 11.25, 'exposureTimeNs': 1302083},
    {'degrees': 22.5, 'exposureTimeNs': 2604166},
    {'degrees': 45, 'exposureTimeNs': 5208333},
    {'degrees': 90, 'exposureTimeNs': 10416666},
    {'degrees': 135, 'exposureTimeNs': 15625000},
    {'degrees': 180, 'exposureTimeNs': 20833333},
    {'degrees': 270, 'exposureTimeNs': 31250000},
    {'degrees': 360, 'exposureTimeNs': 41666666},
]
print('CAMERA_INTERACTION_SNAPSHOT_PASS')
