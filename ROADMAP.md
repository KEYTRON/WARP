# WARP roadmap

Stage: 0.4.8

Stages go in order: `[x]` is done, `[ ]` is planned. The current stage is the first unfinished one.

Design of repositories and nodes: [docs/NODES.md](docs/NODES.md). Also in [Русский](ROADMAP.ru.md) and [Deutsch](ROADMAP.de.md).

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
- [ ] Packages and CI for aarch64 (WARP itself builds and passes its tests on linux/aarch64 in an OrbStack machine on the MacBook; `allan` is published for it; a runner and more packages are still to do)
- [ ] riscv64 — once there is real hardware to test on
- [x] The C library in the platform. glibc, musl (Alpine, Void musl) and bionic (Android) are different ABIs, and Void Linux ships both glibc and musl: today `linux-x86_64` means glibc, so a musl machine would take a build that cannot even start (checked: the dynamic build on Void musl answers "not found"). Decision: the platform learns the C library, detected at run time (the interpreter of `/bin/sh`), not at build time. Builds: `linux-x86_64` (glibc, as before), `linux-x86_64-musl`, and `linux-x86_64-static` for a fully static build that runs on every Linux. The client takes its exact libc first and the static build second; a musl machine never takes a glibc build. Fully static packages (Go, Rust, upstream static builds such as ripgrep, jq, btop) are published once as `static`. WARP itself ships as a static binary (`tools/build-static.sh`: works on Gentoo glibc and Void musl alike). The survey learns the libc too (report schema 3, musl is its own platform). Done in 0.4.6: the client detects the libc, `WARP_LIBC` overrides it, `warp platform --all` lists the candidates; the k1os static packages are tagged `_static` and the top level of the index falls back to them for older clients
- [x] CI runners for the other platforms: native macOS (`macos-arm64`), linux/aarch64 (ten `k1arm-<distro>` runners in the OrbStack machine on the MacBook), and the phone. Every distro now builds on x86_64 and arm through one matrix workflow (`.github/workflows/warp-distros.yml`); the ten per-distro workflows stay as `workflow_dispatch` for debugging one distro alone

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
- [x] A user-owned prefix with no root (`~/.warp` or `/opt/warp`), `warp shellenv` for the shell profile — done: `/opt/warp` on macOS, `$PREFIX` in Termux, `warp shellenv` prints the PATH line
- [x] An installer: one command that explains what it does, waits for Enter, fetches the prebuilt `macos-aarch64` binary and shows the pinned key fingerprint to compare with the site; a `.pkg` for managed installs later — done: `tools/install.sh` (also for Linux and Termux); warp itself fetches the signed index afterwards
- [x] System facts for the survey on macOS (`sysctl`: OS version, CPU, cores, memory) — done
- [x] The first packages built for `macos-aarch64` (ripgrep and jq from the upstream releases; btop publishes no macOS build) and published with a `builds` entry — done
- [x] Portable tests (no GNU-only `tar -I`, `timeout`, `stat -c`): they pass on native macOS and the macOS job runs them (the Docker-based end-to-end test stays on Linux)
- [x] Two CI runners on the MacBook: the native macOS one builds with `make` on Apple Silicon and runs the crypto vectors, the linux/aarch64 one lives in the OrbStack machine and builds the distro images. Docker for the arm builds is installed inside that machine; OrbStack on the MacBook itself is the engine for the native runner
- [x] The node starts at login through a per-user launchd agent (`init/install-service.sh`, no root); `--port` is remembered in `seed.conf` (OrbStack holds 7777 on this Mac); a running node is found without `/proc`
- [ ] Longer term: replace Homebrew on this machine with WARP (the repository becomes the base repository)

## Termux (Android)
WARP was started there by accident (0.4.1 built from source and ran); SSH access to the phone exists, so it can be tested directly.
- [x] Prefix-aware paths: store, binaries and temporary files under `$PREFIX` (no `/var/lib`, no `/usr/local/bin`, no root); the CA bundle at `$PREFIX/etc/tls/cert.pem` (found since 0.4.5) — done: `$PREFIX/var/lib/warp`, `$PREFIX/bin`, `$PREFIX/tmp`
- [x] Installer for Termux (`pkg`-free, prebuilt `android-aarch64` binary) — done: the same `tools/install.sh`
- [ ] The first `android-aarch64` packages (static Go and Rust binaries) — started: `allan` 0.3.4 for `android-aarch64` (built natively in Termux with Go, CGO off, from the same tag as the other platforms; the Termux Go toolchain will become a package once warp replaces `pkg`)
- [x] CI on the phone, started by hand when it is at home and charging, so it does not drain the battery away from home. There is deliberately **no runner on the phone**: the Actions runner is a .NET program and does not run on Android's Bionic libc. The `warp-termux.yml` workflow runs on the lab PC (already on the tailnet) and drives the phone over SSH on port 8022; the build itself is native, `make` with clang, no proot and no glibc

## Documentation in German
- [x] `README.de.md`, `ROADMAP.de.md` and `docs/NODES.de.md` next to the English and Russian ones
- [x] The site offers the German documentation on the project page

## Real time on the site
- [x] The tracker cards (admin page and the public project card) are updated over a WebSocket instead of polling: the tracker pushes a new snapshot when a node announces or reports (polling stays as the fallback while the socket is down)

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
- [x] Building and running in Termux: `warp 0.4.5` builds with clang and reports `os: android`, `arch: aarch64`; `tests/crypto.sh` passes there
- [ ] Delivering K1K services to `/SVC` as WARP packages
