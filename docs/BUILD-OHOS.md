# Building and installing on HarmonyOS

Three stages: cross-compile the host into `libdmhost.so`, assemble a signed HAP that carries it plus the game data, install. A round takes about five minutes end to end; treat that as the unit of work and **batch your changes into it** — one-variable-at-a-time device rounds are the slowest possible way to find out anything.

## 0. What you need

| Item | Why |
|---|---|
| clang with an AArch64 target + an OpenHarmony sysroot | the host is a plain C library; no OHOS NDK C++ runtime is linked in |
| `-lace_napi.z -lhilog_ndk.z -lvulkan` | NAPI entry, logging, and the Vulkan loader |
| DevEco Studio (or the standalone `hvigor` + `hdc` toolchains) | packaging, signing, install |
| A JDK on `PATH` | `hvigorw` is a Node script that shells out to Java |
| A device with developer mode + `hdc` | the only real test platform. Nothing here is emulatable: the whole point is the Maleoon driver's behaviour |
| Your own signing material (certificate + profile) | see step 3. **Never commit them.** |

## 1. Cross-compile the host

```bash
make -f dm/ohos/Makefile solib
# OHOS_SYSROOT defaults to $HOME/ohos/sysroot; CROSS defaults to
#   clang --target=aarch64-linux-ohos --sysroot=$(OHOS_SYSROOT)
```

Targets, in the order they are useful:

- `sweep` — compile every source **individually**, collect and rank the distinct errors. Use this first on a new toolchain; it tells you the whole gap list in one pass instead of one error at a time.
- `obj` — compile all objects, stop at the first failure.
- `link` — link an executable, which is the cheapest way to fish out every undefined symbol at once.
- `solib` — the real product: everything `-fPIC`, then `-shared -Wl,-Bsymbolic`. `-Bsymbolic` is not decoration: the hand-written assembly thunks resolve `adrp`/`add` against local symbols, and without it the dynamic linker can bind them to something else.

The output is ~4.4 MB with symbols. `hvigor` strips it to ~1.5 MB when packaging — which matters in step 3.

Sources compiled: `dm/host/*.c` (incl. the `hle_*` service implementations and the `dm_*` gameplay drivers inherited from the D2R branch), `dm/gfx/**/*.c` (the NVN→Vulkan replay layer lives in `dm/gfx/nvn/nvn_vulkan.c`), `dm/ohos/{front_ohos,audio_ohos,pad_ohos,app_ohos,xcomp_ohos,vk_smoke}.c`, the three `.S` thunk files, `dm/ohos/compat/*`, and a handful of `libdeflate` files. Objective-C/Metal and the iOS shell are not compiled.

## 2. The HAP project

Two modules, because a single HAP cannot carry 5.18 GB of data:

```
entry/   libdmhost.so + the primary romfs (18,086 files, 2.1 GB) + the ArkTS shell
ds2/     the overlay romfs (3,224 files, 2.8 GB)
```

Data goes in `src/main/resources/resfile/romfs/…`, **not** `rawfile`: `resfile` is copied verbatim and is addressable by path, while `rawfile` is compressed and not. Two gotchas we paid for:

- The host probes several candidate mount points in order and takes the first one where the config file is readable: `<module>/resources/resfile/romfs`, `<module>/resources/rawfile/romfs`, `<module>/resources/romfs`, then the sandbox `files/romfs`. See `pick_romfs()` in [dm/ohos/app_ohos.c](../dm/ohos/app_ohos.c#L167).
- The build tool **silently drops dot-files** from packaged resources. The romfs config therefore has to be named `build.config`, not `.build.config` — a "why is the config missing" investigation that is already over.
- The overlay is discovered by walking sibling modules from the bundle root (`romfs_overlay_from_bundle()`), so adding a third data module requires no code change.

## 3. Sign, and install a package that is bigger than the tooling expects

Sign with your own certificate and profile. Two hard-won rules:

- **Use a distinct `bundleName` per port**, with its own signing material. Reusing another project's bundle name makes the two installs fight over the same sandbox, and a profile pins its bundle name — so a profile that works for one project is simply wrong for another.
- `hdc install -r` **truncates large packages and then reports a bogus `verify signature failed`**. This is not a signing problem. Do this instead:

```bash
hdc file send entry-default-signed.hap /data/local/tmp/dstapp/entry.hap
hdc file send ds2-default-signed.hap   /data/local/tmp/dstapp/ds2.hap
hdc shell bm uninstall -n <bundleName>
hdc shell bm install -p /data/local/tmp/dstapp/entry.hap /data/local/tmp/dstapp/ds2.hap
```

**Verify the transfer by byte count on both sides.** A short file produces exactly the signature error described above, and it will look like your signing is broken.

## 4. Self-audit the artefact before installing it

An incorrect package does not fail loudly. It installs, runs the *previous* code, and you lose a whole device round. Two assertions, both cheap:

1. **The HAP must not be older than the `.so`** — proof that the build tool really repackaged rather than reusing a cached output.
2. **The `.so` inside the HAP must contain a string literal that only this round's source has.** Do **not** compare md5s: stripping makes them differ even when the package is perfect. Grep for a log line you just added:

```bash
unzip -p entry-default-signed.hap libs/arm64-v8a/libdmhost.so > /tmp/inhap.so
grep -a -q "排队上传 s=" /tmp/inhap.so && echo green || echo "red: stale .so inside"
```

The marker must be a **runtime string literal**. Put it in a comment and it is stripped, and the check rejects a perfectly good package.

## 5. Launch

```bash
hdc shell aa start -a EntryAbility -b <bundleName> \
  --ps DM_MODULES main,sdk,subsdk0 --ps DM_FS 1 --ps DM_TITLE_DRV 0 \
  --ps DM_SAMPLE 2000 --ps DM_SPUR 500 --ps DM_CRASHDIR <sandbox files dir>
```

Every parameter passed with `--ps` whose name starts with `DM_` is scanned out of the launch JSON and turned into an environment variable inside the host ([dm/ohos/app_ohos.c](../dm/ohos/app_ohos.c)). That is the entire configuration surface — see [DM-SWITCHES.md](DM-SWITCHES.md).

## 6. Driving the game, and reading what happened

Input is injected through the on-screen controller overlay; coordinates are in the device's native 2848×1276 space:

```bash
hdc shell "uitest uiInput click 2572 893"      # A
hdc shell "uitest uiInput click 2406 1058"     # B
hdc shell snapshot_display -f /data/local/tmp/x.jpeg && hdc file recv /data/local/tmp/x.jpeg ./x.jpeg
hdc shell "top -H -b -n1 -p $(hdc shell pidof <bundleName>)"   # idle-spin vs actually-slow
```

The app's own sandbox is **not** readable at the path the app sees. Read the shell-side view instead:

```
/data/app/el2/100/base/<bundleName>/haps/entry/files/
```

That is where `crash_<pid>.txt`, `thr.txt`, `miss.txt` and dumped frames land, and `hdc file recv` works on it.

**Never ship pixels through the log channel.** `hilog` silently discards long lines and bursts — measured: 3,840 base64 chunks emitted by the guest, **zero** arrived, while 4,213 short lines from the same window did, in a capture of 881,568 total lines. Write files instead, and pull them.

## 7. Scripts in `scripts/`

Sanitised versions of what we actually run, with placeholders for the paths, the build host and the bundle name:

| Script | What it does |
|---|---|
| `build_host.sh` | sync sources → cross-compile `libdmhost.so` → copy into the HAP project |
| `pack_install.sh` | build the HAP, run both self-audit assertions, transfer, verify sizes, install |
| `run_world.sh` | launch with a diagnostic switch set and drive EULA → menu → create world → in world, screenshotting at each step |
| `asset_audit.ps1` | walk the romfs and report anomalies (zero-byte files, name collisions, unresolved references) |
| `rgbapng.ps1` | convert a dumped `WxH` RGBA8 frame to PNG (also crop and scale) |

The two PowerShell ones exist because the development host is Windows and there is no usable Python/PIL on it; they use `System.Drawing`, need `-ReferencedAssemblies System.Drawing`, and **the C# bodies must stay ASCII-only** — `Add-Type` reads the script as ANSI, so non-ASCII string literals break compilation in a way that looks like a syntax error.
