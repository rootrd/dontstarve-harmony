# The HAP project (sanitised)

This is the HarmonyOS application shell that carries `libdmhost.so` and the game data. It is
**not complete on purpose** — three things were removed, and you add them back locally:

| Missing | Why | How to get it |
|---|---|---|
| `signingConfigs` in `build-profile.json5` | it holds certificate paths, a key alias and two passwords | DevEco Studio's automatic signing, or point it at your own material. The profile is pinned to one `bundleName`, so change `AppScope/app.json5` first |
| `entry/src/main/resources/resfile/romfs/**`, `ds2/.../resfile/romfs/**` | that is the game | [../docs/UNPACKING.md](../docs/UNPACKING.md) |
| `entry/libs/arm64-v8a/libdmhost.so` | build output | `scripts/build_host.sh` |
| `**/resources/base/media/*.png` | app icons | copy from a fresh DevEco template, or use your own art |

## Local DST 1.30.0 build

Run `python scripts/stage_dst_hap.py` after compiling the native library and preparing
`build/game/update/RomFS`, `build/game/patched`, and `build/game/shaders.bin`.
The generated project is `build/hap/`; open that directory in DevEco for signing.
It contains both resource modules, generated icons, the module manifest and native types.
The staging script preserves local signing configuration on subsequent runs.

See [the current rendering/build report](../docs/OHOS-RENDERING-2026-10.md) for commands,
verified results and current device validation.

## Layout

```
hap/
  AppScope/app.json5              bundleName, version
  build-profile.json5             SDK versions + the two modules (signing emptied)
  entry/                          module 1: the host + the primary romfs (about 2.6 GB)
    src/main/ets/
      entryability/EntryAbility.ets   receives the launch Want, forwards DM_* parameters
      pages/Index.ets                 XComponent surface + the on-screen controller overlay
      model/PadLayout.ets             the overlay geometry
    src/main/cpp/types/libdmhost/     the NAPI declaration set
    src/main/module.json5
  ds2/                            module 2: the overlay romfs (about 2.6 GB), no code of its own
```

## Two modules, one app

A single HAP cannot carry 5.18 GB of data, so the romfs is split. The host discovers the
second root by walking sibling modules from the bundle
(`romfs_overlay_from_bundle()` in [../dm/ohos/app_ohos.c](../dm/ohos/app_ohos.c)) — adding a
third data module needs no code change.

Both data directories must be `resfile`, not `rawfile`: `resfile` is copied verbatim and is
addressable by path, while `rawfile` is accessed through ResourceManager. DST 1.30.0
is detected by `prefabs.xml` and `SplashScreen.tex` at the RomFS root; this version
does not use the CASC `data/.build.config` layout described by older build notes.

## The overlay

`Index.ets` draws the Switch button layout as ArkTS shapes over the XComponent; taps are
mapped to guest controller bits in [../dm/host/hle_hid.c](../dm/host/hle_hid.c). For
automated testing you can bypass it entirely with `hdc shell uitest uiInput click X Y` in
the device's native panel space.

## Debug text

One rule learned here: a bring-up overlay that is centred in a `Stack()` sits exactly on top
of the game's modal dialog. If you add status text, put it in a corner and hide it on
success.
