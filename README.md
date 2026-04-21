# WARP

WARP is a small package manager written in C. It downloads signed package archives, verifies integrity, and manages a local store under `/var/lib/warp`.

The trust model is simple:

- GitHub release metadata is the source of truth for hashes and signatures.
- Package delivery can happen through direct download, torrent, or P2P variants.
- The downloaded bytes are always checked against the published SHA256 before installation.

## Build

```bash
make
```

or:

```bash
./build.sh build
```

## Install

```bash
sudo make install
```

or:

```bash
sudo ./build.sh install
```

By default the binary is installed to `/usr/local/bin/warp`. Set `PREFIX` if you want another location.

## Commands

- `warp search <query>`
- `warp install <package>`
- `warp remove <package>`
- `warp list`
- `warp rollback <package>`
- `warp info <package>`
- `warp update`
- `warp keygen`
- `warp sign <file>`
- `warp pack <directory>`

## Release/index layout

The package index is expected at:

`https://github.com/KEYTRON/WARP/releases/download/packages-v1/index.json`

Package archives are expected to be published from the same repository under release assets.

### Variants

Package entries can optionally define a `variants` array. Each variant can carry:

- `kind`: `direct`, `torrent`, `magnet`, `p2p`, `http`, `https`, or `file`
- `url`: delivery URL or magnet URI
- `sha256`: expected archive hash
- `signature`: optional signature for the variant metadata
- `priority`: higher numbers win within the same transport class

If no `variants` array exists, WARP falls back to the legacy top-level `url` and `sha256` fields.

### Signing

Use `warp sign <file> [privkey_hex]` to create a `<file>.sig` sidecar with a base64 Ed25519 signature. The same keypair from `warp keygen` can be used to sign release metadata such as `index.json`.
