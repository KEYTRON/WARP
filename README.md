# WARP

WARP is a small package manager written in C. It downloads signed package archives, verifies integrity, and manages a local store under `/var/lib/warp`.

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
- `warp pack <directory>`

## Release/index layout

The package index is expected at:

`https://github.com/KEYTRON/WARP/releases/download/packages-v1/index.json`

Package archives are expected to be published from the same repository under release assets.
