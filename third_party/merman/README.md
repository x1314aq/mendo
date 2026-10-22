# Merman prebuilt C ABI (Windows x64)

This directory vendors the prebuilt Windows x64 native library and the
release-matched headers from Merman 0.9.2.

- Upstream: https://github.com/Latias94/merman
- Artifact source: https://pub.dev/api/archives/merman-0.9.2.tar.gz
  (`native/windows/x86_64/merman_ffi.dll`)
- DLL SHA-256: `2017C4FE9784579C6EC45BE31A31E2AA15F8593947948B7FD6ED4A50BAA60B47`
- ABI: Merman native ABI 3
- License: MIT OR Apache-2.0; see `LICENSE-MIT`, `LICENSE-APACHE`, and
  `THIRD_PARTY_NOTICES.md`.

The Flutter-distributed artifact exposes SVG, analysis, ASCII, and both native
layout engines, but intentionally omits PNG/JPEG/PDF. mendo therefore asks
Merman for `svg` output and rasterizes that SVG with the native LunaSVG library.
No Rust toolchain is required to build mendo.
