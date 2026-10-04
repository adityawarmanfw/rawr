# raw_ingress

GPU ingress for camera RAW formats the realtime pipeline can't consume
directly.

- `Raw10Unpacker` (`shaders/raw10_unpack.comp`): reads MIPI RAW10 5-byte
  groups at `y * rowStrideBytes + x / 4 * 5` from the camera buffer, imported
  as a storage buffer, and writes four `r16ui` texels. The app records it into
  its owned R16 slot image in place of the CPU lock/unpack/upload
  (`pipeline/RawCpuUploadPool`), which stays as the fallback.

The host test `tests/native/raw10_unpack_vulkan_test.cpp` checks the shader
against the app's CPU `unpackRaw10Row`, including padded strides.
