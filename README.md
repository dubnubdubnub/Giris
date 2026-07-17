# Giris

KiCad hardware for the Giris project. The boards live under `hw/`.

## Setup

This repo uses git submodules for its shared footprint/3D-model library (`hw/library`) and `hw/marbastlib`. **The 3D bodies will not appear in KiCad's 3D viewer until the submodules are checked out**, because the `.step` files they reference live in `hw/library/packages3d/`.

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/dubnubdubnub/Giris.git
```

If you already cloned without `--recurse-submodules`, initialize them after the fact:

```bash
git submodule update --init --recursive
```

> Note: `hw/marbastlib` may print an error while recursing into a nested `.history` submodule. That is harmless — it is upstream and holds no 3D models the boards need.

## 3D models

Footprint 3D bodies resolve from three locations:

- `${KICAD10_3DMODEL_DIR}/…` — the standard models shipped with KiCad.
- `${KIPRJMOD}/../library/packages3d/…` — this repo's `hw/library` submodule (must be checked out; see Setup).

When adding new 3D models to a board, reference them with one of the path variables above rather than a bare relative path (e.g. `../../packages3d/…`), so they resolve regardless of where the repo is cloned.
