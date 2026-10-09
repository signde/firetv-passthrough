# Rebuilding and replacing Frida

The adjacent source-manifest.json pins the complete collected source inputs.
The companion release archive contains each original archive under archives/,
plus the dependency manifest and license notices. Every archive can be verified
against the manifest before extraction. Sources remain under their upstream licenses.

## Upstream build entry points

Use a supported Linux or macOS build host, Python, Node/npm, a C/C++ compiler,
and Android NDK r29 (`ANDROID_NDK_ROOT`). Frida's compiler backend requires
Go >= 1.26 when enabled. Its Vala compiler is the Frida fork pinned in deps.toml.
Normal build tools and the NDK are obtained separately. Frida can bootstrap its
host toolchain; to replace a library, build that library from source rather than
silently reusing a prebuilt SDK containing the original copy.

For a checkout with upstream Git metadata and submodule layout:

```sh
git clone https://github.com/frida/frida.git
cd frida
git checkout 324ee3a9c90bf398f9b6571ec16c27c70a062839
git submodule update --init --recursive
export ANDROID_NDK_ROOT=/path/to/android-ndk-r29
./configure --host=android-arm --without-prebuilds=sdk \
  -Dinject=enabled -Dfrida_tools=disabled -Dfrida_python=disabled
make
```

`--without-prebuilds=sdk` is the pinned configure script's option for building SDK
dependencies from source. The provided archives allow recovering/editing those
same source revisions independently of GitHub availability. Git metadata is used
by upstream for version detection; preserve it when overlaying/editing sources.
npm/Go lockfiles identify additional package inputs supplied in the companion.
Select your modified dependency source in the relevant Meson subproject before
building; use a clean build directory to avoid reusing an old static archive.

Alternatively, upstream's explicit SDK builder is:

```sh
python3 releng/deps.py build --bundle sdk --host android-arm
```

It places dependency checkouts in its work directory and writes a rebuilt SDK
archive under the dependency cache (`FRIDA_DEPS`, or the checkout's deps/).
Its source code and per-package options are included. Inspect the generated build
configuration and ensure modified libraries are selected before relinking.
These are source-based rebuild instructions; a full Frida rebuild was not run
as part of the module's packaging verification.

## Use a rebuilt runtime in this module

1. Keep all applicable upstream notices and source available with your distribution.
2. In this module's build.py, change RUNTIME_SHA to the SHA256 of your rebuilt
   ARM32 frida-inject. This explicitly acknowledges the changed trusted runtime.
3. Run `python3 build.py --runtime /path/to/rebuilt/frida-inject`.
   This regenerates payload.sha256, including the changed runtime and build script.
4. Install the resulting ZIP in Magisk and reboot. Source changes may require
   revalidation of Frida API compatibility and playback behavior.

No signature or secret key is needed. The standard release pins the official
runtime for tested behavior; the source and integrity checks are editable for
modified builds. Do not replace only the executable in a signed-off package
without regenerating its integrity manifest.
