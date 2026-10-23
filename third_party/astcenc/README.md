# astcenc — vendored header + how the OHOS statics are produced

Upstream: https://github.com/ARM-software/astc-encoder, **version 5.7.0**, Apache License 2.0
(verbatim text in [LICENSE.txt](LICENSE.txt), retained here as the licence requires).

What is in the repository, and what is not:

| Path | In git? | What it is |
|---|---|---|
| `include/astcenc.h` | yes | the upstream public header, the only one `libdmhost.so` compiles against |
| `LICENSE.txt` | yes | upstream Apache-2.0 text |
| `ohos/*.a` | **no** (gitignored) | build products — ~14 MB of cross-compiled static libraries |

The `.a` files are deliberately not published: they are a toolchain output, not source, and
anyone who wants them can produce byte-identical inputs from the upstream tarball.

## Building the OHOS statics

Cross-compile the upstream CMake project for `aarch64-linux-ohos` with the OpenHarmony NDK
clang. NEON is the only ISA asked for (the target is aarch64), the CLI and the tests are off,
and the library is what gets linked into `libdmhost.so` so the device can re-encode
BC1/BC3/BC4/BC5 blocks into ASTC at load time instead of paying for a Vulkan BC path the
Maleoon 920 does not have.

```sh
# astcenc 5.7.0 source at $ASTCENC_SRC, OHOS sysroot at $HOME/ohos, NDK C++ headers at $HOME/ohos/cxx
cmake -S "$ASTCENC_SRC" -B astc-ohos-build -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=astc-ohos-toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
  -DASTCENC_ISA_NATIVE=OFF -DASTCENC_ISA_NEON=ON -DASTCENC_ISA_NONE=OFF \
  -DASTCENC_CLI=OFF -DASTCENC_UNITTEST=OFF -DASTCENC_WERROR=OFF \
  -DASTCENC_DIAGNOSTICS=OFF -DASTCENC_INVARIANCE=OFF -DASTCENC_X86_GATHERS=OFF
cmake --build astc-ohos-build -j 8
# -> libastcenc-neon-static.a, plus libc++_static.a / libc++abi.a from the NDK runtime
```

The toolchain file sets `CMAKE_<C,CXX>_COMPILER_TARGET=aarch64-linux-ohos`, points
`CMAKE_SYSROOT` at the OHOS sysroot and adds `-isystem <ndk>/c++/v1 -fPIC` — the C++ standard
library headers are *not* in the sysroot, so that `-isystem` is the whole trick.

Consumer side: `dm/tools/bc2astc.c` (offline PC baking) and
`dm/gfx/nvn/nvn_rt_astc.c` (the on-device runtime path) are the two places that call this API.
