# Third-party components

Mini Tracker itself is MIT-licensed (see `LICENSE`). It bundles the following
freely-redistributable third-party code/assets:

| Component | Use | License |
|---|---|---|
| **SQLite** (amalgamation) | embedded database | Public Domain |
| **stb_image** | PNG/JPEG decoding for box art | Public Domain / MIT (dual) |
| **JetBrains Mono** | UI font (rasterized into `src/font.h`) | SIL Open Font License 1.1 — `third_party/font/OFL.txt` |

Notes:
- The font is rasterized to a bitmap atlas at build time (`tools/genfont.py`); the
  OFL permits embedding and redistribution of the rendered glyphs.
- The app links the device's own `libc`/kernel interfaces only via syscalls; no
  proprietary device libraries are bundled (the binary is fully static musl).
