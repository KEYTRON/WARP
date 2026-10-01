# Vendored code

WARP does not link OpenSSL. Ed25519 comes from **Monocypher 4.0.2** (Loup Vaillant,
dual-licensed 2-clause BSD / CC0, see `MONOCYPHER-LICENCE.md`), the optional
`monocypher-ed25519` part that implements the standard Ed25519 of RFC 8032 (SHA-512).
Monocypher had an independent audit (Cure53, 2018).

Files are copied unchanged from the release tarball
<https://monocypher.org/download/monocypher-4.0.2.tar.gz>
(`src/monocypher.{c,h}`, `src/optional/monocypher-ed25519.{c,h}`). They are byte-identical
to the git tag `4.0.2` (https://github.com/LoupVaillant/Monocypher) except the tarball
replaces the `__git__` version label on the first line with `4.0.2`.

SHA-256 (`../sha256.c`) is WARP's own, written from FIPS 180-4.

sha256 of the vendored files:

```
afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59  monocypher.c
f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374  monocypher.h
7c9b16056cbd27521919e8a6f56a228808b9e718afc42e3d33f28c08e5abdee2  monocypher-ed25519.c
bd546edcd468d64e28caa3dbf4b1d6bfad7435c0ce994723fd81aae26405121b  monocypher-ed25519.h
```

`tests/crypto.sh` checks the whole thing against RFC 8032 vectors, NIST SHA-256 vectors and,
when the `openssl` command is available, against OpenSSL (both directions: signed by one,
verified by the other).
