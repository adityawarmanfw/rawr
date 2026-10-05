# TinyDNG reader + writer

Source: https://github.com/syoyo/tinydng
Revision: 1f181699511e08e51baabd9fdab2311ea1253d4b (C11 v3).
Writer source: https://github.com/syoyo/tinydng/blob/release/tinydng_write.c
(same upstream repo, release branch; C11 v3 API-compatible streaming/tiled
writer with uncompressed/LZW/PackBits/lossless-JPEG payloads).
MIT; see LICENSE.

Local changes: retain fractional black levels, default crop, and bounded DNGPrivateData
in the metadata model. The legacy integer black levels remain ABI-source compatible.

Local writer extensions (Rawr full-DNG parity, same MIT file): `tinydng_write.c`
additionally emits ActiveArea/DefaultCrop/DefaultScale, UniqueCameraModel,
rational BlackLevel, BackwardVersion, dual-illuminant color + forward +
calibration matrices, AnalogBalance, BaselineExposure, LensInfo, serials,
RawDataUniqueID, NoiseProfile, OpcodeList1/2/3 (re-serialized from generic
opcodes), DNGPrivateData, IFD0 descriptive tags, Orientation, and a staged
EXIF sub-IFD. `tinydng_exif`/`tinydng_raw_info`/`tinydng_write_image` carry
additive Rawr extension fields (writer-emitted; reader-accepted where parsed).
Opcode-list allocation includes all four 32-bit header fields per opcode.

Local fix: RATIONAL/SRATIONAL writers pick a per-value power-of-ten denominator
(<= 1e6) instead of a fixed 1e6, so values above ~4295 (e.g. 16-bit black
levels) are written exactly instead of saturating at UINT32_MAX/1e6.
