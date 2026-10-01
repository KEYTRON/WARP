#!/usr/bin/env python3
"""Regenerate and sign a WARP package index from the archives in a directory.

    tools/make-index.py <packages-dir> --base-url URL --key priv.hex [--warp ./warp]

Translated descriptions come from `descriptions.json` next to the archives
({"ripgrep": {"ru": "...", "de": "..."}}); the English text stays the entry's
`description`, the others go into a `descriptions` map the client and the site pick from.

Every `name-version-<tag>.warp` in the directory becomes a build of its package
with its real sha256 and size; descriptions are carried over from the existing
index.json when present. <tag> names the platform: the architecture alone for
Linux (x86_64, aarch64: the names that already exist) and <os>_<arch> for other
systems (android_aarch64, macos_aarch64); `noarch` is a build for any platform.
A package built for more than linux-x86_64 gets a `builds` map keyed by
`<os>-<arch>` (plus `-musl` or `-static` for a build that is not for glibc); the top level then still describes the linux-x86_64 build, so
clients that predate `builds` keep working. Clients install only the build made
for their own platform. Entries whose archive is missing are dropped — an index must
never advertise what the mirrors cannot serve. The detached signature
(`index.json.sig`) is produced with `warp sign` so it matches what clients
verify.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import date
from pathlib import Path

NAME_RE = re.compile(r"^(?P<name>[a-z0-9_+.-]+?)-(?P<version>\d[^-]*)-(?P<arch>[a-z0-9_]+)\.warp$")
OSES = ("android", "macos", "freebsd", "openbsd", "netbsd", "windows")
LEGACY = "linux-x86_64"


LIBCS = ("musl", "static")      # glibc is the default and has no suffix: linux-x86_64


def platform_of(tag: str) -> str:
    """Archive tag -> platform: x86_64 -> linux-x86_64, x86_64_static -> linux-x86_64-static,
    x86_64_musl -> linux-x86_64-musl, android_aarch64 -> android-aarch64, macos_aarch64 -> macos-aarch64."""
    if tag == "noarch":
        return "any"
    libc = ""
    for suffix in LIBCS:
        if tag.endswith("_" + suffix):
            tag, libc = tag[: -(len(suffix) + 1)], "-" + suffix
            break
    for os_name in OSES:
        if tag.startswith(os_name + "_"):
            return f"{os_name}-{tag[len(os_name) + 1:]}{libc}"
    return f"linux-{tag}{libc}"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("packages_dir", type=Path)
    ap.add_argument("--base-url", required=True, help="mirror URL the archives are served from")
    ap.add_argument("--key", required=True, type=Path, help="Ed25519 private key (hex) for warp sign")
    ap.add_argument("--warp", default="warp", help="warp binary with the `sign` command")
    ap.add_argument("--peer-list-url", default="https://keytron-prime.org/warp/peers")
    ap.add_argument("--keep-old-versions", action="store_true",
                    help="also list every older release as name@version (so `warp install name@version` and pins can use it)")
    ap.add_argument("--no-deltas", action="store_true", help="do not build <name>-<old>-to-<new>-<arch>.warpdelta files")
    args = ap.parse_args()

    index_path = args.packages_dir / "index.json"
    old = {}
    if index_path.exists():
        try:
            old = json.loads(index_path.read_text()).get("packages", {})
        except json.JSONDecodeError:
            pass

    # name -> platform -> [(version, archive, tag)]
    translations = {}
    tr_file = args.packages_dir / "descriptions.json"
    if tr_file.exists():
        translations = json.loads(tr_file.read_text(encoding="utf-8"))

    found: dict[str, dict[str, list[tuple[str, Path, str]]]] = {}
    for archive in sorted(args.packages_dir.glob("*.warp")):
        m = NAME_RE.match(archive.name)
        if not m:
            print(f"skip {archive.name}: name is not name-version-arch.warp", file=sys.stderr)
            continue
        found.setdefault(m["name"], {}).setdefault(platform_of(m["arch"]), []).append((m["version"], archive, m["arch"]))

    def vkey(v: str):
        return [int(p) if p.isdigit() else p for p in re.split(r"[.\-]", v)]

    base = args.base_url.rstrip("/")

    def build_of(name: str, version: str, archive: Path, tag: str, older: list) -> dict:
        b = {
            "version": version,
            "sha256": sha256(archive),
            "size": archive.stat().st_size,
            "url": f"{base}/{archive.name}",
        }
        # Deltas from every older archive of the same platform still on the mirror.
        deltas = []
        if not args.no_deltas:
            for old_version, old_archive, _ in older:
                delta = args.packages_dir / f"{name}-{old_version}-to-{version}-{tag}.warpdelta"
                if not delta.exists():
                    subprocess.run([args.warp, "delta", str(old_archive), str(archive), str(delta)],
                                   check=True, stdout=subprocess.DEVNULL)
                dsize = delta.stat().st_size
                if dsize >= b["size"] * 0.9:
                    print(f"  delta {old_version}->{version} saves nothing ({dsize} B), not listed")
                    delta.unlink()
                    continue
                deltas.append({
                    "from_version": old_version,
                    "from_sha256": sha256(old_archive),
                    "url": f"{base}/{delta.name}",
                    "sha256": sha256(delta),
                    "size": dsize,
                })
                print(f"  delta {old_version}->{version}: {dsize} B ({100 * dsize / b['size']:.0f}% of full)")
        if deltas:
            b["deltas"] = deltas
        return b

    packages = {}
    for name, platforms in sorted(found.items()):
        for lst in platforms.values():
            lst.sort(key=lambda t: vkey(t[0]))
        # The plain key is the latest release of each platform (with deltas).
        selections = [(None, {plat: (lst[-1], lst[:-1]) for plat, lst in platforms.items()})]
        if args.keep_old_versions:
            # Every release also gets a `name@version` entry so it can be installed
            # or pinned later; clients that predate this ignore the extra keys.
            def is_latest_everywhere(v):      # the plain name already covers it
                return all(lst[-1][0] == v for lst in platforms.values() if any(t[0] == v for t in lst))
            for v in sorted({v for lst in platforms.values() for v, _, _ in lst}, key=vkey):
                if is_latest_everywhere(v):
                    continue
                selections.append((v, {plat: ([t for t in lst if t[0] == v][0], [])
                                       for plat, lst in platforms.items() if any(t[0] == v for t in lst)}))

        for only_version, chosen in selections:
            builds = {plat: build_of(name, ver, arch, tag, older)
                      for plat, ((ver, arch, tag), older) in sorted(chosen.items())}
            # older clients read the top level as the glibc x86_64 build; a fully static one runs there too
            legacy = builds.get(LEGACY) or builds.get(LEGACY + "-static")
            first = legacy or next(iter(builds.values()))
            entry = {
                "version": first["version"],
                "description": old.get(name, {}).get("description", f"{name} {first['version']} for K1OS"),
            }
            deps = old.get(name, {}).get("deps")
            if deps:
                entry["deps"] = deps
            tr = translations.get(name) or old.get(name, {}).get("descriptions") or {}
            tr = {lang: text for lang, text in tr.items() if lang != "en" and text}
            if tr and only_version is None:
                entry["descriptions"] = tr
            if legacy:      # the top level stays the linux-x86_64 (or static) build for older clients
                entry.update({k: legacy[k] for k in ("sha256", "size", "url", "deltas") if k in legacy})
            if set(builds) != {LEGACY}:
                entry["builds"] = builds
            key = name if only_version is None else f"{name}@{only_version}"
            packages[key] = entry
            plats = ",".join(sorted(builds))
            print(f"{key:<12} {entry['version']:<10} {first['size']:>10} B  {first['sha256'][:12]}  [{plats}]")

    dropped = sorted(set(old) - set(found))
    if dropped:
        print(f"dropped (no archive on the mirror): {', '.join(dropped)}", file=sys.stderr)

    index = {
        "timestamp": date.today().isoformat(),
        "signature": "",
        "peer_list_url": args.peer_list_url,
        "packages": packages,
    }
    index_path.write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n")

    subprocess.run([args.warp, "sign", str(index_path), str(args.key)], check=True)
    sig = index_path.with_suffix(".json.sig")
    if not sig.exists() or not sig.read_text().strip():
        print("signing produced no index.json.sig", file=sys.stderr)
        return 1
    print(f"wrote {index_path} and {sig.name} ({len(packages)} packages)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
