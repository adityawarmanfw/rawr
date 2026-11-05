# Photo lens metadata

Rawr builds `ExifIFD:LensMake` and `ExifIFD:LensModel` from the captured frame's
Camera2 metadata, without a phone/lens-name database. This applies to single-frame
and merged DNGs and capture JPEGs (including Ultra HDR).

Example:

```text
LensMake:  vivo
LensModel: vivo X300 Ultra back camera 22.48mm f/2.67
```

## Identity and optics

- The name combines the existing manufacturer and marketing model. If the model
  already starts with the manufacturer (case-insensitive, whole-word prefix),
  the manufacturer is not repeated. Only lens-name strings are normalized;
  `Make`, `Model`, hardware matching and DNG `UniqueCameraModel` are unchanged.
- Known Camera2 `LENS_FACING` adds `front`, `back` or `external` before `camera`.
  Unknown facing omits that word; it is never assumed to mean back. Optical
  categories such as wide or telephoto are not inferred or included.
- The focal length printed in the name is the single **physical** focal length
  reported in the pinned camera's `LENS_INFO_AVAILABLE_FOCAL_LENGTHS`, not the
  shot's focal length, 35mm equivalent or digital-zoom-adjusted length.
- For that fixed-focal module, the name uses the widest supported opening: the
  smallest f-number in `LENS_INFO_AVAILABLE_APERTURES`. Stopping down does not
  rename the lens. Missing aperture characteristics omit the aperture suffix.
- Missing or multiple available focal lengths produce a `camera` name with
  known facing but without numeric suffixes. Independent focal/aperture lists
  do not establish a zoom lens's endpoint specification; no current-shot fallback
  is used.
- Numeric `FocalLength` and `FNumber` still describe the shot. The numeric
  35mm-equivalent calculation also retains its existing per-frame input.
- Numbers use the shortest round-trip representation of the reported float,
  independent of locale; integer f-numbers retain `.0`. For example, a reported
  `6.765` or `1.78` is not rounded to one decimal or padded with invented digits.
- EXIF lens identity strings are printable ASCII; non-ASCII/control bytes are
  replaced with `?`.
- Native LensMake uses the camera OEM as the integrated-module brand, as in
  the inspected Apple/Google convention. It does not identify the actual optical
  supplier. No vendor-marketing brand or native lens serial is inferred.

Missing optical specifications are omitted from the name; no optics are guessed.
Missing shot results do not remove known static name details.
`FocalLengthIn35mmFormat` is omitted when it cannot be calculated within EXIF's
valid range, including when sensor dimensions are unavailable/invalid. This
does not remove known facing or optics from the name. Digital crop and
reconstructed pixel scale do not change the module's lens name. This is a descriptive identity policy, not a requirement
that Exif model strings contain these suffixes or proof of an OEM lens model.
Native OEM-name discovery and supplied-name preference are not implemented.

## Optical specification and source preservation

Native DNG writes both Exif LensSpecification (`0xA432`) and DNG IFD0 LensInfo
(`0xC630`). Native JPEG writes LensSpecification too. All describe the shortest
and longest physical focal lengths followed by the widest opening at each
endpoint, not the selectable aperture range. Fixed-focal modules repeat their
widest opening. Independent lists do not establish zoom-endpoint pairings, so
unproven endpoint apertures use the defined rational `0/0`. A specification
without usable focal information is omitted; other zero rationals are unchanged.

JPEG also writes ApertureValue from the actual shooting aperture and known
MaxApertureValue from available optical characteristics. Both use unsigned APEX
units; unknown or unrepresentable values are omitted. A stopped-down shot never
supplies a guessed maximum opening.

Imported-DNG development and DNG recompression/downscaling retain supported
source lens names, physical/equivalent focal lengths, FNumber, explicit aperture
values, known maximum aperture and lens serial. Exif specification precedence is
RAW-image Exif, root Exif, then DNG-only LensInfo fallback. Each four-value
record is selected whole; endpoints from conflicting records are never spliced.
Explicit source aperture tags take precedence over recalculation. DNG conversion
preserves the unsigned rational components of FNumber, FocalLength and
MaxApertureValue, including components above the signed 32-bit range. DNG conversion
also retains the original DNG LensInfo; its root/SubIFD fallback is optical-only,
not a wholesale replacement of unrelated EXIF. Source serials are retained as
capture facts and may identify a lens. Missing source facts never come from the
exporting phone. Focus/distance, zoom, subject coordinates and focal-plane tags
are outside this preservation extension.

## Performance and provenance

String generation and equivalent-focal-length calculation run only in the
existing DNG/JPEG encoder workers, after acquisition. JPEG processing resolves
a compact optional focal-length/maximum-opening snapshot and copies the
sensor-size pair and facing from the frame's pinned camera context. The encoder
uses the optical characteristics and facing for the name and the already frozen
shot focal length for the numeric equivalent. No new device queries, disk/network I/O, locks,
waits or work are added to shutter submission, Camera2 callbacks or preview
processing. Encoder placement is not a claim of zero resource cost.

The widest characteristic aperture is resolved once per output preparation and
shared by the name, specification and maximum-opening calculation. The numeric
equivalent is calculated separately from naming; no classification is performed.
DNG moves generated name strings into its output parameters; JPEG borrows imported names
only during serialization and keeps generated native names in caller-owned
storage for the same lifetime. No persistent cache or synchronization is added.
These remove source-level redundant work; no latency or battery improvement
has been measured.

Capture recovery reconstructs the transient JPEG input from persisted frame
metadata; the capture-journal format is unchanged. Re-rendering imported DNGs
preserves the source's lens name, make and 35mm-equivalent tag, including absent
tags, rather than generating metadata for the phone performing the export.
