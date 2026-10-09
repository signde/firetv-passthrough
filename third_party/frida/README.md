# Frida 17.22.2 redistribution inventory

This module distributes the unmodified official Android ARM32 frida-inject binary,
SHA256 `6a6d539f09cc2ed2679b8bdea3ce2cb343224adc6887d9fb227b5d1f41bebf07`.
Our MIT license does not relicense that binary or its components.

## Review scope and source provenance

The inventory is a conservative source/dependency superset, not a linker-produced
SBOM. It follows the Frida 17.22.2 superproject, its pinned core/gum/releng commits,
releng's 20261004 SDK manifest, nested build submodules, compiler/barebone npm
lockfiles and compiler go.mod/go.sum. Optional features and build-only packages
are retained rather than assumed absent from the official binary.

Each archive has a pinned revision/version, download URL and SHA256 in
source-manifest.json. npm integrity and Go h1 module sums were also verified.
The NDK r29 notices were read from Google's official archive; its LLVM identity
matches the Android compiler string present in our executable. The NDK is a
separately obtained standard compiler toolchain, not included in the module.
The source companion includes build tools and optional esbuild package inputs
as well as library source archives.

## License handling

| Components | Handling |
| --- | --- |
| Frida core/gum | wxWindows exception and upstream notices retained |
| GLib, libiconv, libgee, JSON-GLib, libsoup, glib-networking, TinyCC/libtcc, libsepol | LGPL texts, original notices and pinned source archives supplied |
| elfutils libraries | LGPLv3-or-later option documented in library headers; full sources and license texts supplied |
| libdwarf, libnice, libbpf | Original component-specific LGPL/BSD/MPL notices and sources retained; no assumption that one top-level license covers every file |
| V8, Capstone, QuickJS, OpenSSL, libffi, compression/network libraries | Original permissive-license notices retained, including vendored notices |
| SQLite/libselinux | Upstream public-domain statements retained |
| JavaScript and Go packages | Original notices, lockfile metadata and verified package/module archives supplied |
| Android NDK runtime | Official NDK NOTICE and NOTICE.toolchain included |

Complete source archives are offered alongside the module ZIP on the same GitHub
release as `frida-17.22.2-source-and-notices.tar.gz`, with a checksum. This is actual
source delivery, not merely a link to a moving upstream branch or an offer to
provide source later. Keep that asset available for every published binary release.
The archive contains the full Frida source and library sources for rebuilding and
relinking, including upstream modifications at the pinned revisions. This project
does not modify Frida itself.

Users may modify/rebuild the libraries and relink Frida, and may reverse engineer
it to debug those modifications. Module checksum guards are integrity checks, not
restrictions on those rights; BUILDING.md explains replacing the runtime and
regenerating guards. No signing keys or manufacturer authorization are needed
to install a rebuilt module through Magisk on an already-rooted supported device.

## Specific notice details

- @frida/crypto 1.0.1 declares MIT and credits Frida Developers in package.json,
  but ships no separate LICENSE file. Its metadata and MIT terms are preserved
  in PACKAGE-NOTICE.txt; no copyright year was invented.
- undici-types 5.26.5 lacks a separate license in its package. The matching
  upstream Undici v5.26.5 MIT license is included.
- libdwarf's LIBDWARFCOPYRIGHT/LGPL.txt and embedded notices are retained in
  addition to its top-level COPYING file.
- The included Go runtime license is the upstream Go 1.26.0 BSD text. This does
  not assert that the official executable used that exact Go patch release.
- The source bundle includes files licensed differently from their parent
  project, including build/test files. Per-file notices remain authoritative.

## Verification limits

We verified archive hashes, package integrity, notice inclusion, package manifests,
and module/runtime identity. We have not independently rebuilt the entire official
Frida executable or established a byte-for-byte reproduction of its release build.
The rebuild instructions follow the pinned upstream build scripts and are supplied
with the source. This inventory documents the engineering review, not a legal
opinion or a claim that every optional component is linked into the executable.
