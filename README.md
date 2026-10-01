# WARP

[Русская версия](README.ru.md) · [Deutsche Version](README.de.md)

WARP is a small package manager written in C for K1OS and a handful of other
distributions. It downloads signed package archives, verifies them, and
manages a versioned local store under `/var/lib/warp` with instant rollback.

![Tests](https://github.com/KEYTRON/WARP/actions/workflows/warp-tests.yml/badge.svg)
![K1OS](https://github.com/KEYTRON/WARP/actions/workflows/warp-k1os.yml/badge.svg)
![Alpine](https://github.com/KEYTRON/WARP/actions/workflows/warp-alpine.yml/badge.svg) ![AlmaLinux](https://github.com/KEYTRON/WARP/actions/workflows/warp-almalinux.yml/badge.svg) ![Arch](https://github.com/KEYTRON/WARP/actions/workflows/warp-arch.yml/badge.svg) ![Artix](https://github.com/KEYTRON/WARP/actions/workflows/warp-artix.yml/badge.svg) ![Devuan](https://github.com/KEYTRON/WARP/actions/workflows/warp-devuan.yml/badge.svg) ![Fedora](https://github.com/KEYTRON/WARP/actions/workflows/warp-fedora.yml/badge.svg) ![Ubuntu](https://github.com/KEYTRON/WARP/actions/workflows/warp-ubuntu.yml/badge.svg) ![Void glibc](https://github.com/KEYTRON/WARP/actions/workflows/warp-void.yml/badge.svg) ![Void Musl](https://github.com/KEYTRON/WARP/actions/workflows/warp-void-musl.yml/badge.svg)

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

WARP needs a C compiler, `make` and the libcurl headers, and nothing else: it has no
OpenSSL dependency (SHA-256 is its own, Ed25519 is the vendored Monocypher in `src/vendor/`).
On macOS the Command Line Tools are enough. `tools/build-static.sh` builds a fully static
binary (musl, Docker needed) that runs on glibc and musl systems alike.

## Install

```bash
sudo make install       # or sudo ./build.sh install
```

The binary goes to `/usr/local/bin/warp`; set `PREFIX` for another location.

Without a compiler, on Linux, macOS (Apple Silicon) or Termux, use the installer. It
explains what it will do, waits for Enter, downloads the ready-made binary over HTTPS,
checks its sha256, lets `warp update` fetch the signed index, and prints the key warp
trusts so it can be compared with the one on the project page:

```bash
curl -fsSL https://keytron-prime.org/packages/install.sh | sh
```

Where warp lives depends on the system: `/var/lib/warp` and `/usr/local/bin` on Linux
(root), `/opt/warp` on macOS (made yours once with `sudo`, so nothing needs root after that;
add `eval "$(warp shellenv)"` to the shell profile), `$PREFIX` in Termux (no root).

## Commands

| Command | What it does |
|---|---|
| `warp update` | Refresh the index of every enabled repository (verifying signatures) |
| `warp search <query>` | Search available packages |
| `warp install <pkg>` | Install a package (`repo/pkg` to pick a repository explicitly) |
| `warp upgrade [pkg...]` | Upgrade installed packages, via delta when one is published |
| `warp remove <pkg>` | Remove a package |
| `warp rollback <pkg>` | Go one version back through the activation history (repeat for more) |
| `warp versions` / `switch` / `pin` / `unpin` / `run` / `gc` | Versions side by side, see below |
| `warp list` / `warp info <pkg>` | Installed packages / details |
| `warp repo list\|add\|remove\|enable\|disable` | Manage repositories |
| `warp delta <old> <new> <out>` | Build a binary delta between two archives |
| `warp delta-apply <old> <delta> <out>` | Rebuild the new archive from the old one and a delta |
| `warp keygen [priv pub]` | Generate an Ed25519 signing keypair |
| `warp sign <file> [priv]` | Write a detached base64 signature `<file>.sig` |
| `warp verify <file> --pubkey <hex>` | Check `<file>.sig` (Ed25519) against a public key |
| `warp pubkey <private-key-file>` | Print the public key that belongs to a private key |
| `warp sha256 <file>...` | Print SHA-256 sums (like `sha256sum`) |
| `warp pack <dir>` | Create a `.warp` archive from a directory |
| `warp seed` | Run the node: seed installed packages to peers (P2P transport) |
| `warp volunteer` | Same node, plus cache rarely seeded packages within your limits |
| `warp stats` | This node and the network: data sent, packages, nodes online, traffic |

## Seeding, volunteer mode and statistics

There is one node process (`warp seed`, service `warp-seed`). Volunteer mode is
a setting of that node, not a second service, so the two can never fight over
port 7777.

```bash
warp volunteer --setup               # interactive: disk, package count, SD-card mode, monthly cap
warp volunteer --quota 10.46G        # disk for the cache: 10G, 10.46G, 500M or all
warp volunteer --packages 46         # at most 46 packages (or all)
warp volunteer --reserve 2G          # always keep 2 GiB of the disk free (default 1G)
warp volunteer --disable             # back to plain seeding; a running node applies it at once
```

With limits, the node fills the cache with the least seeded packages first. With
`--quota all` it takes everything. A running node re-reads its settings when
`warp volunteer ...` changes them (SIGHUP) and looks for rarely seeded
packages again every six hours.

`warp stats` shows what this node has done (bytes and packages sent, cache,
monthly traffic) and the state of the network (nodes online, packages, data
moved). `warp --help` ends with a one-line summary taken from local files only.

**Anonymous statistics are off until you say yes.** The first interactive
`warp seed` / `warp volunteer` shows exactly what would be sent and asks
`[y/N]`; Enter means no. `warp stats --what` shows it again, `warp stats
--consent` / `--no-stats` change the answer, `--reset-id` makes a new random
node id. The report carries a random node id, the warp version, the OS and
architecture, and a coarse hardware profile like a hardware survey (kernel as
major.minor, distribution and version, CPU model and core count, memory rounded
to a standard size); it all feeds the public platform survey: percentages only,
one count per node per month. It also says whether volunteer mode is on, bytes and packages sent, and how
many packages the node can seed — no file names, no paths, no package
contents. Seeding itself still tells the tracker your address, port and the
names of the packages you seed (peers need that to find you); that is separate
from the statistics. Network totals are the sum of what consenting nodes
report and are not verified.

When a new version adds fields to the report, the earlier answer does not cover them: the node sends nothing until you have seen the new report (`warp stats --consent`).

## Versions side by side

Every installed version lives in the store as `store/<name>-<hash12>`; one of
them is active. Several can sit next to each other, so a program that needs an
older version (or not yet a newer one) keeps working.

```bash
warp versions tool            # installed in the store, and published in the index
warp install tool@1.2         # that release, next to the current one; it becomes active and is pinned
warp switch tool 2.0          # any installed version, no download
warp pin tool [1.2]           # `warp upgrade` leaves it alone (a version also switches to it)
warp unpin tool
warp rollback tool            # one step back through the activation history; repeat to go further
warp run tool@1.1 -- args     # run a stored version without switching (--bin <name> picks the program)
warp gc [--keep N] [--dry-run]   # remove versions that are not active, pinned or among the last N activations
```

An older release is chosen on purpose, so `warp install tool@1.2` pins it:
without that the next `warp upgrade` would undo it. A pin follows an explicit
`switch` or `rollback`. `warp rollback` walks back through every activation,
not just between the last two. Old releases are installable when the
repository publishes them: `tools/make-index.py --keep-old-versions` adds a
`name@version` entry for each older release next to the plain `name` (the latest);
clients that predate this ignore the extra entries. Not covered yet: different
programs using different versions of the same *dependency* at once (Nix-style
closures); see the roadmap.

## Platforms: prebuilt packages only

WARP never downloads source and compiles it. Every package is built in CI for
each platform it supports, and the signed index lists one **build** per
platform, written `<os>-<arch>`: `linux-x86_64`, `linux-aarch64`,
`android-aarch64` (Termux), `macos-aarch64`. The client installs only the build
made for its own OS and CPU (`warp platform` prints it). If a package has no
build for your platform, `warp install` says so and lists the platforms it has;
it does not fall back to a binary for another system. Architecture-independent
packages (scripts, data) are published once as `any`.

Archives are named `name-version-<arch>.warp` on Linux (x86_64, aarch64) and
`name-version-<os>_<arch>.warp` elsewhere (`android_aarch64`); `noarch` means
any platform. `tools/make-index.py` groups them by platform.

The platform also has a C library part, read from the machine at run time (the
interpreter of `/bin/sh`; `WARP_LIBC=glibc|musl` overrides it). Builds are tagged
`linux-x86_64` (glibc), `linux-x86_64-musl` and `linux-x86_64-static` (runs anywhere).
A machine takes the build for its own libc first and a static one after it; a musl
machine never takes a glibc build. `warp platform --all` lists the candidates. Archives
are named `name-version-x86_64_musl.warp` and `name-version-x86_64_static.warp`.

Package descriptions come in the user's language when the index has them: an
entry may carry a `descriptions` map (`{"ru": "...", "de": "..."}`) next to the
English `description`, and `warp search` / `warp info` pick the text the way
gettext does (`WARP_LANG`, then the locale, where `C` means English, then
`LANGUAGE`). `tools/make-index.py` reads the translations from a
`descriptions.json` in the repository directory.

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

`warp update` exits with code 1 when at least one enabled repository could
not be updated (the signature did not verify or no mirror answered). Packages
from the other repositories stay available.

### The first repository: `keytron`

The first public repository besides the built-in `k1os` is `keytron` on
[keytron-prime.org](https://keytron-prime.org). Add it with one command:

```bash
sudo warp repo add keytron https://keytron-prime.org/packages/keytron --pubkey 53d0a36597b812873cdaa42b11b08592ac3f0998998ccb7e59d6833640f9d883
sudo warp update
```

You can check the key against the published
[`pubkey.hex`](https://keytron-prime.org/packages/keytron/pubkey.hex).
The signing key of this repository is not stored on the serving host.

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

A package built for more than `linux-x86_64` carries a `builds` map keyed by
platform. Each build has its own `version`, `sha256`, `size`, `url` and
`deltas` (a build may omit `version` and use the entry's); the top level then
still describes the `linux-x86_64` build, so clients that predate `builds` keep
working. A package published only for `linux-x86_64` keeps the plain format above.

```json
"tool": {
  "description": "...",
  "version": "1.0", "sha256": "…", "size": 1234, "url": "https://…/tool-1.0-x86_64.warp",
  "builds": {
    "linux-x86_64":    {"version": "1.0", "sha256": "…", "size": 1234, "url": "https://…/tool-1.0-x86_64.warp"},
    "linux-aarch64":   {"version": "1.0", "sha256": "…", "size": 1180, "url": "https://…/tool-1.0-aarch64.warp"},
    "android-aarch64": {"version": "1.0", "sha256": "…", "size": 1190, "url": "https://…/tool-1.0-android_aarch64.warp"}
  }
}
```

`variants` is optional: `kind` is `direct`, `torrent`, `magnet`, `p2p`, `http`,
`https` or `file`; higher `priority` wins within a transport class. Without it
the top-level `url`/`sha256` are used. `index.json.sig` is the base64 Ed25519
signature of the raw `index.json` bytes (`warp sign index.json`), not a field
inside the JSON — a signature cannot cover a document that contains itself.

The optional `peer_list_url` field is this repository's P2P tracker. Without
it, the repository's packages are fetched only from its mirrors and installs
are not reported anywhere. The compiled-in tracker `keytron-prime.org/warp`
is used only for the built-in `k1os`.

## Tests

```bash
sh tests/delta-roundtrip.sh        # delta build/apply, wrong base and truncation rejected
sh tests/crypto.sh              # SHA-256 and Ed25519: RFC/NIST vectors, forgeries, OpenSSL as the oracle
sh tests/versions.sh              # versions side by side: install any, switch, pin, rollback chain, run, gc
sh tests/multi-platform.sh        # one build per OS/CPU in the index; the client takes only its own
sh tests/node-stats.sh             # node: volunteer limits, one process, counters, opt-in statistics
sh tests/e2e-delta-upgrade.sh      # custom repo over HTTP, install → upgrade via delta → rollback, in the K1OS container
```

The second test needs Docker and `ghcr.io/keytron/k1os:latest`; both run in
CI (`warp-tests.yml`).
