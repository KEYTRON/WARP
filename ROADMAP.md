# WARP roadmap

Stage: 0.4.1

Stages go in order: `[x]` is done, `[ ]` is planned. The current stage is the first unfinished one.

Design of repositories and nodes: [docs/NODES.md](docs/NODES.md).

## The base package manager
- [x] Install, remove, list and inspect packages
- [x] Versioned local store with instant rollback
- [x] Search across available packages
- [x] Building `.warp` archives (`warp pack`)

## Trust model
- [x] Trust only a pinned Ed25519 key
- [x] Signed index, fetched together with its signature from the same mirror
- [x] sha256 check of every archive and delta against the index
- [x] Key generation and signing (`warp keygen`, `warp sign`)

## Repositories and updates
- [x] Multiple repositories with pinned keys
- [x] `warp upgrade` for installed packages
- [x] Binary delta updates (WARPDLT1 format)
- [x] Dependencies in the signed index (pinned by version)
- [x] Volunteer seeding (`warp seed`, `warp volunteer`) with a disk quota and a monthly upload cap

## Tested on distributions
- [x] Delta tests and an e2e install → upgrade → rollback test
- [x] CI on K1OS, Alpine, AlmaLinux, Arch, Artix, Devuan, Fedora, Ubuntu, Void and Void musl

## Rollback protection
- [ ] A version number and an expiry date on the index
- [ ] The client rejects an index older than the one it has seen or an expired one

## Repository mode
- [ ] `warp repo init` and `warp publish` instead of an external script
- [ ] Release notes when publishing a version (`warp publish --notes`), stored in the signed index
- [ ] Split keys: root (offline), index and online

## Node mode
- [ ] Node certificates with an expiry date, signed by the repository
- [ ] Nodes join by agreement of both sides and sync all packages or a share of them
- [ ] Automatic renewal, availability checks and node revocation
- [ ] The client downloads from nodes and fails over
- [ ] Namespace delegation to nodes (if needed)

## Other architectures
All packages are built for x86_64 today, and the architecture exists only in the archive name.
- [x] Platform in the index (`builds` per `<os>-<arch>`): the client takes only the build for its own OS and CPU and never compiles
- [ ] Packages and CI for aarch64 (WARP itself builds and passes its tests on linux/aarch64 in an OrbStack machine on the MacBook; a runner and published packages are still to do)
- [ ] riscv64 — once there is real hardware to test on
- [ ] The C library in the platform. glibc, musl (Alpine, Void musl) and bionic (Android) are different ABIs, and Void Linux ships both glibc and musl: today `linux-x86_64` means glibc, so a musl machine would take a build that cannot even start (checked: the dynamic build on Void musl answers "not found"). Decision: the platform learns the C library, detected at run time (the interpreter of `/bin/sh`), not at build time. Builds: `linux-x86_64` (glibc, as before), `linux-x86_64-musl`, and `linux-x86_64-static` for a fully static build that runs on every Linux. The client takes its exact libc first and the static build second; a musl machine never takes a glibc build. Fully static packages (Go, Rust, upstream static builds such as ripgrep, jq, btop) are published once as `static`. WARP itself ships as a static binary (`tools/build-static.sh`: works on Gentoo glibc and Void musl alike). The survey and the site switchers learn the libc too
- [ ] CI runners for the other platforms (each becomes its own workflow, so the CI block on the site shows it): native macOS (on the MacBook), linux/aarch64 (the OrbStack machine on the MacBook is ready), Termux (none yet; to be started by hand when the phone is at home and charging, so it does not drain the battery away from home)

## Versions side by side (idea from the user, 2026-10-01)
Today several versions already sit in the store (`store/<name>-<hash12>`), but only one `prev` link exists: rollback toggles between two.
- [x] Install any version: `warp install name@1.2`, `warp versions name` (installed and available); the index keeps old versions (`name@version`)
- [x] `warp switch name <version>` to any installed version; rollback walks back through an activation history, not a single slot
- [x] `warp pin name [version]` / `unpin`: `warp upgrade` leaves a pinned package alone (for a program that needs a version no newer than X, or no older)
- [x] `warp run name@1.2 -- args`: start a specific version without switching the active one
- [x] `warp gc`: remove versions that are not active, pinned or in the recent history (the disk is limited)
- [ ] Different dependents using different versions of a dependency at once (the Nix closure model): needs packages that find their dependencies by store path; after the dependency resolver

## macOS (native)
Checked on Apple Silicon without Homebrew: WARP builds with the Command Line Tools alone, links only the system `libcurl` and `libSystem`, knows it is `macos-aarch64`, verifies a real signed index and refuses a package without a macOS build with a clear message.
- [x] No OpenSSL (own SHA-256, vendored Ed25519), so nothing but the Command Line Tools is needed to build
- [x] The CA bundle is looked up per system (macOS keeps it in `/etc/ssl/cert.pem`)
- [ ] A user-owned prefix with no root (`~/.warp` or `/opt/warp`), `warp shellenv` for the shell profile
- [ ] An installer: one command that explains what it does, waits for Enter, fetches the prebuilt `macos-aarch64` binary and shows the pinned key fingerprint to compare with the site; a `.pkg` for managed installs later
- [ ] System facts for the survey on macOS (`sysctl`: OS version, CPU, cores, memory)
- [ ] The first packages built for `macos-aarch64` (ripgrep, jq, btop from the upstream releases) and published with a `builds` entry
- [ ] Portable tests (no GNU-only `tar -I`, `timeout`, `stat -c`)
- [ ] Two CI runners on the MacBook: a native macOS runner and the linux/aarch64 one (the OrbStack machine, already prepared)
- [ ] Longer term: replace Homebrew on this machine with WARP (the repository becomes the base repository)

## Termux (Android)
WARP was started there by accident (0.4.1 built from source and ran); SSH access to the phone exists, so it can be tested directly.
- [ ] Prefix-aware paths: store, binaries and temporary files under `$PREFIX` (no `/var/lib`, no `/usr/local/bin`, no root); the CA bundle at `$PREFIX/etc/tls/cert.pem` (found since 0.4.5)
- [ ] Installer for Termux (`pkg`-free, prebuilt `android-aarch64` binary)
- [ ] The first `android-aarch64` packages (static Go and Rust binaries)
- [ ] A CI runner on the phone that is started by hand when it is at home and charging, so it does not drain the battery away from home

## Documentation in German
- [ ] `README.de.md`, `ROADMAP.de.md` and `docs/NODES.de.md` next to the English and Russian ones; the site then offers German documentation on the project page too

## Real time on the site
- [ ] The tracker cards (admin page and the public project card) are updated over a WebSocket instead of polling: the tracker pushes a new snapshot when a node announces or reports

## Package automation
- [ ] A recipe per package in a separate repository: where the version comes from, how to verify it (upstream checksum or signature), how to build it, where the license is
- [ ] A scheduled version watcher (like nvchecker / Anitya) comparing recipes with the index
- [ ] A new version is built in CI, installed in a container and proposed as a PR with the upstream changes
- [ ] Vulnerabilities from OSV.dev: a PR for a version with a known CVE is marked urgent
- [ ] Signing the index stays with a human: automation can never ship a package on its own

## Next
- [ ] Dependency resolution on top of the signed index
- [ ] Package licenses: read before installing (from the index) and after (from the installed package), e.g. `warp license <package>`
- [ ] Optional package components (e.g. CUDA, ROCm, Vulkan backends): one package, the client installs only the ones matching the hardware, `--with` / `--without` to choose by hand
- [ ] What's new in a version: release notes in the signed index, shown by `warp upgrade` and `warp info` before upgrading and by `warp changelog <package>` afterwards
- [ ] Working K1OS index mirrors on GitLab and GitVerse
- [ ] zstd compression
- [ ] Declarative system description (`system.yaml`)
- [ ] Building and running in Termux
- [ ] Delivering K1K services to `/SVC` as WARP packages
