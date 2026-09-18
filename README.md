# WARP

[Русская версия](README.ru.md)

WARP is a small package manager written in C for K1OS and a handful of other
distributions. It downloads signed package archives, verifies them, and
manages a versioned local store under `/var/lib/warp` with instant rollback.

![Tests](https://github.com/KEYTRON/WARP/actions/workflows/warp-tests.yml/badge.svg)
![K1OS](https://github.com/KEYTRON/WARP/actions/workflows/warp-k1os.yml/badge.svg)
![Alpine](https://github.com/KEYTRON/WARP/actions/workflows/warp-alpine.yml/badge.svg) ![AlmaLinux](https://github.com/KEYTRON/WARP/actions/workflows/warp-almalinux.yml/badge.svg) ![Arch](https://github.com/KEYTRON/WARP/actions/workflows/warp-arch.yml/badge.svg) ![Artix](https://github.com/KEYTRON/WARP/actions/workflows/warp-artix.yml/badge.svg) ![Devuan](https://github.com/KEYTRON/WARP/actions/workflows/warp-devuan.yml/badge.svg) ![Fedora](https://github.com/KEYTRON/WARP/actions/workflows/warp-fedora.yml/badge.svg) ![Ubuntu](https://github.com/KEYTRON/WARP/actions/workflows/warp-ubuntu.yml/badge.svg) ![Void](https://github.com/KEYTRON/WARP/actions/workflows/warp-void.yml/badge.svg) ![Void Musl](https://github.com/KEYTRON/WARP/actions/workflows/warp-void-musl.yml/badge.svg)

[View all WARP workflow runs](https://github.com/KEYTRON/WARP/actions) — every
workflow runs on the self-hosted K1 lab runner.

## Trust model — "everyone is an enemy"

WARP assumes every network hop is hostile and trusts exactly one thing: an
Ed25519 key you have pinned locally.

- A repository is a base URL (plus optional mirrors) and a **public key**. The
  key is pinned when the repository is added; it never travels with the data.
- Each mirror serves `index.json` and its detached signature `index.json.sig`
  side by side. WARP downloads both **from the same mirror** and verifies the
  signature with the pinned key *before* caching or reading the index. No valid
  signature — no index; there is no unsigned mode.
- The signed index is the closed world: package name, version, URL, size and
  `sha256` of every archive and every delta. Downloaded bytes are checked
  against the index before anything is installed. Mirrors, torrents and P2P
  peers are transport only — they cannot introduce a package or change a hash.
- Dependencies are also stated in the signed index (see below); the manifest
  inside an archive is informative and must agree with the index.

## Build

```bash
make            # or ./build.sh build
```

## Install

```bash
sudo make install       # or sudo ./build.sh install
```

The binary goes to `/usr/local/bin/warp`; set `PREFIX` for another location.

## Commands

| Command | What it does |
|---|---|
| `warp update` | Refresh the index of every enabled repository (verifying signatures) |
| `warp search <query>` | Search available packages |
| `warp install <pkg>` | Install a package (`repo/pkg` to pick a repository explicitly) |
| `warp upgrade [pkg...]` | Upgrade installed packages, via delta when one is published |
| `warp remove <pkg>` | Remove a package |
| `warp rollback <pkg>` | Revert to the previously installed version |
| `warp list` / `warp info <pkg>` | Installed packages / details |
| `warp repo list\|add\|remove\|enable\|disable` | Manage repositories |
| `warp delta <old> <new> <out>` | Build a binary delta between two archives |
| `warp delta-apply <old> <delta> <out>` | Rebuild the new archive from the old one and a delta |
| `warp keygen [priv pub]` | Generate an Ed25519 signing keypair |
| `warp sign <file> [priv]` | Write a detached base64 signature `<file>.sig` |
| `warp pack <dir>` | Create a `.warp` archive from a directory |
| `warp seed` / `warp volunteer` | Seed installed packages to peers (P2P transport) |

## Repositories

The built-in repository `k1os` (mirrors on GitHub, GitLab and GitVerse, key
compiled into the binary) is always present and cannot be removed. Additional
repositories live in `/var/lib/warp/repos.json`, each with its own cache under
`/var/lib/warp/repos/<name>/`.

```bash
warp repo add lab https://mirror.example/lab --pubkey <hex64> [--mirror <url>]...
warp repo add lab https://mirror.example/lab --pubkey-file lab.pub
warp repo list
warp repo disable lab      # keep it configured, skip it
warp repo remove lab
```

The public key is **required** at `add` time: WARP has no notion of an
untrusted repository. Repository names match `[a-z0-9_-]`; when two enabled
repositories publish the same package name, the one listed first wins, and
`warp install other/pkg` selects a specific one.

### Publishing your own repository

Any static HTTP host works. Put the `.warp` archives in a directory and run

```bash
warp keygen repo.priv repo.pub                       # once
tools/make-index.py <dir> --base-url https://mirror.example/lab --key repo.priv
```

`make-index.py` lists the archives actually present (real `sha256` and size —
entries whose archive is missing are dropped, so the index never advertises
what the mirror cannot serve), builds deltas from older archives still in the
directory, and signs the result with `warp sign`. Upload the directory as is;
hand out `repo.pub` to your users.

The built-in `k1os` index and archives live in the `packages` branch of
`KEYTRON/K1OS` (Git LFS for the `.warp` files), served from:

1. `https://github.com/KEYTRON/K1OS/raw/packages`
2. `https://gitlab.com/KEYTRON/K1OS/-/raw/packages`
3. `https://gitverse.ru/keytron46/K1OS/raw/branch/packages`

## Upgrades and delta updates

`warp upgrade` refreshes the indexes and compares the `sha256` of each
installed archive with the index. When the index lists a delta whose
`from_sha256` matches the archive you already have in the store, WARP downloads
only the delta, checks its own hash, rebuilds the new archive locally with
`delta-apply`, and then verifies the result against the full archive's
`sha256` from the index — a delta can never yield bytes the index did not sign
off on. If no delta matches, or the mirror does not serve it, WARP falls back
to the full archive. The previous version stays in the store for `rollback`.

Deltas are content-defined: archives are cut into variable chunks with a
gear-hash rolling boundary (2–64 KiB, ~16 KiB on average), so an insertion at
the front of a file shifts nothing else. Format `WARPDLT1`: header with old and
new size + `sha256`, then a stream of `COPY(offset, len)` / `LIT(bytes)` ops.
For the deltas to pay off the archives must compress reproducibly — pack them
with `gzip -n --rsyncable` (as `tools/make-package.sh` and `make-index.py` do);
a plain `gzip` scrambles the whole stream after the first changed byte.

Typical numbers from the test suite: a 2 MiB archive with 50 KiB changed →
~100 KiB delta. Deltas only pay off for archives that are large relative to
the change — a 40 KiB archive whose code was mostly rewritten (warp 0.3.3 →
0.4.0) saves nothing, so `make-index.py` does not list deltas that are 90 % or
more of the full archive.

## Dependencies

Dependencies live in the **signed index**, not in the archive:

```json
"deps": [ {"name": "openssl", "version": "3.3.2"}, {"name": "libfoo", "version": "1.2", "repo": "lab"} ]
```

Each dependency is pinned by name and exact version (hence exact `sha256`)
within the same repository unless `repo` names another one explicitly — a
repository cannot pull in packages from a repository it does not name, and a
package cannot be satisfied by a "similar" version from an untrusted source.
The manifest inside an archive may repeat the list for humans; if it disagrees
with the index, installation is refused. Resolution over this closed world is
the next step on the roadmap; today the client parses and shows `deps`.

## Index format

```json
{
  "timestamp": "2026-09-19",
  "packages": {
    "warp": {
      "version": "0.4.0",
      "description": "...",
      "sha256": "…", "size": 6104096,
      "url": "https://…/warp-0.4.0-x86_64.warp",
      "deltas": [
        {"from_version": "0.3.3", "from_sha256": "…", "url": "https://…/warp-0.3.3-to-0.4.0-x86_64.warpdelta", "sha256": "…", "size": 180439}
      ],
      "deps": [],
      "variants": [ {"kind": "torrent", "url": "magnet:?…", "sha256": "…", "priority": 10} ]
    }
  }
}
```

`variants` is optional: `kind` is `direct`, `torrent`, `magnet`, `p2p`, `http`,
`https` or `file`; higher `priority` wins within a transport class. Without it
the top-level `url`/`sha256` are used. `index.json.sig` is the base64 Ed25519
signature of the raw `index.json` bytes (`warp sign index.json`), not a field
inside the JSON — a signature cannot cover a document that contains itself.

## Tests

```bash
sh tests/delta-roundtrip.sh        # delta build/apply, wrong base and truncation rejected
sh tests/e2e-delta-upgrade.sh      # custom repo over HTTP, install → upgrade via delta → rollback, in the K1OS container
```

The second test needs Docker and `ghcr.io/keytron/k1os:latest`; both run in
CI (`warp-tests.yml`).
