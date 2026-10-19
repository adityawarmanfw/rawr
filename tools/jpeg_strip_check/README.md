# jpeg_strip_check

Host check that the strip-parallel JPEG encoder (`app/src/main/cpp/encoding/jpeg/StripJpegEncoder.cpp`) decodes to
exactly the pixels of a one-thread `tj3Compress8`, and how much faster it is.

```sh
cmake -S native/libjpeg-turbo -B tmp/ljt-build -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=0
cmake --build tmp/ljt-build --target turbojpeg-static -j8
c++ -std=c++17 -O2 -Inative/libjpeg-turbo/src -Itmp/ljt-build \
  tools/jpeg_strip_check/jpeg_strip_check.cpp app/src/main/cpp/encoding/jpeg/StripJpegEncoder.cpp \
  tmp/ljt-build/libturbojpeg.a -o tmp/jpeg_strip_check
tmp/jpeg_strip_check [some.jpg]
```

## uhdr_fit_check

Compares `UltraHdrMapFit.h` against its committed version on real photos (map bytes that differ, timings):

```sh
git show HEAD:app/src/main/cpp/encoding/jpeg/UltraHdrMapFit.h \
  | sed 's/namespace rawrcam::encoding::jpeg {/namespace oldfit {/; s/const color::/const rawrcam::color::/g' \
  > tmp/UltraHdrMapFit_old.h
c++ -std=c++17 -O2 -DOLD_FIT_HEADER="\"$PWD/tmp/UltraHdrMapFit_old.h\"" -Iapp/src/main/cpp -Inative/gainmap/include \
  -I/opt/homebrew/include -Inative/libjpeg-turbo/src -Itmp/ljt-build \
  tools/jpeg_strip_check/uhdr_fit_check.cpp tmp/ljt-build/libturbojpeg.a -o tmp/uhdr_fit_check
tmp/uhdr_fit_check photo.jpg
```
