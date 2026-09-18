# WARP

WARP is a small package manager written in C. It downloads signed package archives, verifies integrity, and manages a local store under `/var/lib/warp`.

![Alpine](https://github.com/KEYTRON/WARP/actions/workflows/warp-alpine.yml/badge.svg) ![AlmaLinux](https://github.com/KEYTRON/WARP/actions/workflows/warp-almalinux.yml/badge.svg) ![Arch](https://github.com/KEYTRON/WARP/actions/workflows/warp-arch.yml/badge.svg) ![Artix](https://github.com/KEYTRON/WARP/actions/workflows/warp-artix.yml/badge.svg) ![Devuan](https://github.com/KEYTRON/WARP/actions/workflows/warp-devuan.yml/badge.svg) ![Fedora](https://github.com/KEYTRON/WARP/actions/workflows/warp-fedora.yml/badge.svg) ![Ubuntu](https://github.com/KEYTRON/WARP/actions/workflows/warp-ubuntu.yml/badge.svg) ![Void](https://github.com/KEYTRON/WARP/actions/workflows/warp-void.yml/badge.svg) ![Void Musl](https://github.com/KEYTRON/WARP/actions/workflows/warp-void-musl.yml/badge.svg)
![K1OS](https://github.com/KEYTRON/WARP/actions/workflows/warp-k1os.yml/badge.svg)

[View all WARP workflow runs](https://github.com/KEYTRON/WARP/actions)

The trust model is simple:

- GitHub release metadata is the source of truth for hashes and signatures.
- Package delivery can happen through direct download, torrent, or P2P variants.
- The downloaded bytes are always checked against the published SHA256 before installation.
- Torrent/P2P are transport-only paths; they do not replace the GitHub-published hash and signature checks.
- If a peer-sourced transfer stalls, WARP falls back to direct download.

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
- `warp keygen [privkey_hex pubkey_hex]`
- `warp sign <file>`
- `warp pack <directory>`

## Release/index layout

The package index and the archives live in the `packages` branch of
`KEYTRON/K1OS` (Git LFS for the `.warp` files) and are fetched through the
mirror list compiled into `src/main.c`, in order:

1. `https://github.com/KEYTRON/K1OS/raw/packages`
2. `https://gitlab.com/KEYTRON/K1OS/-/raw/packages`
3. `https://gitverse.ru/keytron46/K1OS/raw/branch/packages`

Each mirror must serve `index.json`, `index.json.sig` and the archives side by
side. `tools/make-index.py <packages-dir> --base-url <mirror> --key <priv.hex>`
regenerates the index from the archives actually present (real sha256 and
size, entries without an archive are dropped) and signs it with `warp sign`.

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

Use `warp keygen [privkey_hex pubkey_hex]` to write the signing keypair to custom paths when you do not want to store release keys under `/root`.

WARP fetches `index.json` and its detached signature from the same mirror,
side by side:

- `index.json`
- `index.json.sig`

`index.json.sig` is the base64 Ed25519 signature of the raw `index.json`
bytes (as produced by `warp sign index.json`), not a field inside the JSON
itself — a signature can't cover a document that already contains that same
signature as one of its own fields. If a mirror serves an `index.json`
without a matching, valid `index.json.sig`, WARP refuses to trust it.
