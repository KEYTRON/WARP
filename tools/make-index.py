#!/usr/bin/env python3
"""Regenerate and sign a WARP package index from the archives in a directory.

    tools/make-index.py <packages-dir> --base-url URL --key priv.hex [--warp ./warp]

Every `name-version-arch.warp` in the directory becomes an entry with its real
sha256 and size; descriptions are carried over from the existing index.json
when present. Entries whose archive is missing are dropped — an index must
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
    ap.add_argument("--peer-list-url", default="https://keytron-prime.org/warp/dashboard")
    ap.add_argument("--keep-old-versions", action="store_true", help="list every archive, not just the newest per package")
    ap.add_argument("--no-deltas", action="store_true", help="do not build <name>-<old>-to-<new>-<arch>.warpdelta files")
    args = ap.parse_args()

    index_path = args.packages_dir / "index.json"
    old = {}
    if index_path.exists():
        try:
            old = json.loads(index_path.read_text()).get("packages", {})
        except json.JSONDecodeError:
            pass

    found: dict[str, list[tuple[str, Path]]] = {}
    for archive in sorted(args.packages_dir.glob("*.warp")):
        m = NAME_RE.match(archive.name)
        if not m:
            print(f"skip {archive.name}: name is not name-version-arch.warp", file=sys.stderr)
            continue
        found.setdefault(m["name"], []).append((m["version"], archive))

    def vkey(v: str):
        return [int(p) if p.isdigit() else p for p in re.split(r"[.\-]", v)]

    base = args.base_url.rstrip("/")
    packages = {}
    for name, versions in sorted(found.items()):
        versions.sort(key=lambda t: vkey(t[0]))
        chosen = versions if args.keep_old_versions else versions[-1:]
        for version, archive in chosen:
            key = name if len(chosen) == 1 else f"{name}@{version}"
            entry = {
                "version": version,
                "description": old.get(name, {}).get("description", f"{name} {version} for K1OS"),
                "sha256": sha256(archive),
                "size": archive.stat().st_size,
                "url": f"{base}/{archive.name}",
            }
            deps = old.get(name, {}).get("deps")
            if deps:
                entry["deps"] = deps
            # Deltas from every older archive still on the mirror to this one.
            if not args.no_deltas and version == versions[-1][0]:
                deltas = []
                for old_version, old_archive in versions[:-1]:
                    m = NAME_RE.match(archive.name)
                    delta = args.packages_dir / f"{name}-{old_version}-to-{version}-{m['arch']}.warpdelta"
                    if not delta.exists():
                        subprocess.run([args.warp, "delta", str(old_archive), str(archive), str(delta)],
                                       check=True, stdout=subprocess.DEVNULL)
                    dsize = delta.stat().st_size
                    if dsize >= entry["size"] * 0.9:
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
                    print(f"  delta {old_version}->{version}: {dsize} B ({100 * dsize / entry['size']:.0f}% of full)")
                if deltas:
                    entry["deltas"] = deltas
            packages[key] = entry
            print(f"{key:<12} {version:<10} {entry['size']:>10} B  {entry['sha256'][:12]}")

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
