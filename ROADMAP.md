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
- [ ] Architecture in the index: the client picks the build for its own machine
- [ ] Packages and CI for aarch64 (a runner on a MacBook with Apple Silicon)
- [ ] riscv64 — once there is real hardware to test on

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
