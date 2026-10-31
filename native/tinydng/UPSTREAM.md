# TinyDNG reader + writer

Source: https://github.com/syoyo/tinydng
Revision: 1f181699511e08e51baabd9fdab2311ea1253d4b (C11 v3).
Writer source: https://github.com/syoyo/tinydng/blob/release/tinydng_write.c
(same upstream repo, release branch; C11 v3 API-compatible streaming/tiled
writer with uncompressed/LZW/PackBits/lossless-JPEG payloads).
MIT; see LICENSE.

Local changes: retain fractional black levels, default crop, and bounded DNGPrivateData
in the metadata model. The legacy integer black levels remain ABI-source compatible.

GPS extension: bounded `tinydng_gps` records retain supported unsigned rational
pairs, references and optional presence. A non-recursive GPS-directory reader
caps entries and payload lengths. The writer stages GPS in its existing reserved
header, with a fixed 512-byte value buffer; no pixel rewrite or additional flush.
GPS output uses version 2.4.0.0. Method identifiers/padding and ASCII bytes are
validated; malformed optional count overflow does not discard valid coordinates.
Unknown-type GPS pointers participate in invalid/duplicate-directory detection.
ExifVersion input requires UNDEFINED/4 before its bytes can supply version context.
ASCII staging copies directly into owned writer storage without a redundant heap
temporary.
Coordinate-rational validation is shared with the application metadata model
through a C-compatible helper; validation remains at both input boundaries.

Local writer extensions (Rawr full-DNG parity, same MIT file): `tinydng_write.c`
additionally emits ActiveArea/DefaultCrop/DefaultScale, UniqueCameraModel,
rational BlackLevel, BackwardVersion, dual-illuminant color + forward +
calibration matrices, AnalogBalance, BaselineExposure, DefaultBlackRender, LensInfo, serials,
RawDataUniqueID, NoiseProfile, DNG camera profile tags (HueSatMap 1/2, LookTable,
ProfileToneCurve, encodings, ProfileCopyright, BaselineExposureOffset), OpcodeList1/2/3 (re-serialized from generic
opcodes), DNGPrivateData, IFD0 descriptive tags, Orientation, and a staged
EXIF sub-IFD. `tinydng_exif`/`tinydng_raw_info`/`tinydng_write_image` carry
additive Rawr extension fields (writer-emitted; reader-accepted where parsed).
Opcode-list allocation includes all four 32-bit header fields per opcode.

Local fix: RATIONAL/SRATIONAL writers pick a per-value power-of-ten denominator
(<= 1e6) instead of a fixed 1e6, so values above ~4295 (e.g. 16-bit black
levels) are written exactly instead of saturating at UINT32_MAX/1e6.

Local fix: lossless JPEG SSSS 16 (residual -32768) carries no appended bits
(T.81 H.1.2.2, DNG). The encoder wrote 16 and the decoder read 16, so tinydng
round-tripped its own streams while Adobe/LibRaw desynced for the rest of the
tile. 14-bit input never reaches SSSS 16; 16-bit merged DNGs do at hard edges.

Converter carry (DNG-to-DNG rewrites): the reader now also parses the EXIF
identity/exposure tags the writer emits (FNumber, FocalLength, SubjectDistance,
MaxApertureValue, DigitalZoomRatio, ExposureCompensation, Flash, LightSource,
SensingMethod, ExposureMode, WhiteBalance, Sharpness, FocalLength35mm,
ExifVersion, DateTimeOriginal/Digitized, OffsetTime*, SubSecTime*, body/lens
serials, LensSpecification), UniqueCameraModel, DNGBackwardVersion,
BaselineExposure, TIFF-EPStandardID, BlackLevelRepeatDim, and MakerNote/XMP
as opaque bounded blobs (2MB). Rationals fill both the signed pair and the
unsigned copy the writer uses. The writer additionally emits LinearizationTable,
ProfileName, ProfileEmbedPolicy, TIFF-EPStandardID, XMP (IFD0), and
ExposureCompensation, Sharpness, MakerNote and the source ExifVersion (ExifIFD).

Local fix: TD_TAG_DEFAULT_BLACK_RENDER was 50713 (BlackLevelRepeatDim), so the
reader stored BlackLevelRepeatDim[0] as DefaultBlackRender and the writer could
emit an invalid value 2. It is now 51110 and only 0/1 are written.

Local fix: the TIFF Predictor tag is always 1 for lossless JPEG; the Annex H
selector lives in the JPEG stream and values > 1 mean differencing/floating
point to TIFF readers.

Local layout: the streaming writer reserves the metadata block (main extras,
EXIF sub-IFD, main IFD) right after the TIFF header at create and streams
pixel data after it; finish fills the block in place. Android's media scanner
does not reach an IFD written after ~150MB of 200MP pixel data, which left
gallery rows without dimensions or orientation.

Local fix: RATIONAL/SRATIONAL values use the smallest exact power-of-two or
power-of-ten denominator when one exists (matrices are typically n/128), and
the 1e6 fallback rounds instead of truncating.
