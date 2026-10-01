#!/bin/sh
# SHA-256 and Ed25519 are WARP's own now (no OpenSSL): official test vectors, sizes
# around every block boundary, forgery attempts, and, when the `openssl` command is
# there, a differential test in both directions (signed by one, verified by the other).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
W="${WARP:-$ROOT/warp}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }
cd "$T"

# ── SHA-256: NIST vectors ──
sum() { "$W" sha256 "$1" | cut -d' ' -f1; }
: > empty
[ "$(sum empty)" = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 ] || fail "SHA-256 of the empty message"
printf 'abc' > abc
[ "$(sum abc)" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad ] || fail "SHA-256 of abc"
printf 'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq' > two
[ "$(sum two)" = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1 ] || fail "SHA-256 of the 448-bit message"
python3 -c "import sys; sys.stdout.buffer.write(b'a' * 1000000)" > million
[ "$(sum million)" = cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0 ] || fail "SHA-256 of a million a"
ok "SHA-256 matches the NIST vectors (empty, abc, 448 bits, a million 'a')"

# ── SHA-256: every size around the 64-byte block and the padding edge ──
for n in 1 55 56 57 63 64 65 119 120 121 127 128 129 1000 65535 65536 65537 1048576; do
    head -c "$n" /dev/urandom > blob
    want="$(python3 -c "import hashlib,sys; print(hashlib.sha256(open('blob','rb').read()).hexdigest())")"
    [ "$(sum blob)" = "$want" ] || fail "SHA-256 differs from Python's for $n bytes"
done
ok "SHA-256 agrees with an independent implementation for 18 sizes around the block edges"

# ── Ed25519: RFC 8032 section 7.1 ──
rfc() { # rfc <seed> <pub> <msg-hex> <sig-hex>
    printf '%s\n' "$1" > rfc.priv
    [ "$("$W" pubkey rfc.priv)" = "$2" ] || fail "public key for seed $1"
    python3 -c "import sys; sys.stdout.buffer.write(bytes.fromhex('$3'))" > rfc.msg
    "$W" sign rfc.msg rfc.priv >/dev/null || fail "sign"
    got="$(python3 -c "import base64; print(base64.b64decode(open('rfc.msg.sig').read()).hex())")"
    [ "$got" = "$4" ] || fail "signature differs from the RFC vector for $1"
    "$W" verify rfc.msg --pubkey "$2" >/dev/null || fail "the RFC signature does not verify"
}
rfc 9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60 \
    d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a "" \
    e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b
rfc 4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb \
    3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c 72 \
    92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00
ok "Ed25519 reproduces the RFC 8032 test vectors (public key, signature, verification)"

# ── key files ──
"$W" keygen k.priv k.pub >/dev/null
[ "$(stat -c %a k.priv 2>/dev/null || stat -f %Lp k.priv)" = 600 ] || fail "private key must be 0600"
[ "$("$W" pubkey k.priv)" = "$(cat k.pub)" ] || fail "pubkey does not match keygen"
"$W" keygen k2.priv k2.pub >/dev/null
[ "$(cat k.priv)" != "$(cat k2.priv)" ] || fail "two keygen runs gave the same key"
ok "keygen: private key 0600, public key derivable, keys differ between runs"

# ── forgeries ──
head -c 5000 /dev/urandom > doc
"$W" sign doc k.priv >/dev/null
"$W" verify doc --pubkey "$(cat k.pub)" >/dev/null || fail "a good signature must verify"
python3 - <<'PY'
import base64, shutil
sig = bytearray(base64.b64decode(open("doc.sig").read()))
L = 2**252 + 27742317777372353535851937790883648493
def save(name, raw): open(name, "w").write(base64.b64encode(bytes(raw)).decode())
flipped = bytearray(sig); flipped[10] ^= 1; save("doc.flip.sig", flipped)         # one bit of R
flipped = bytearray(sig); flipped[40] ^= 1; save("doc.flipS.sig", flipped)        # one bit of S
s = int.from_bytes(sig[32:], "little") + L                                         # S + L: same point, not canonical
mall = bytearray(sig[:32]) + bytearray(s.to_bytes(32, "little")); save("doc.mall.sig", mall)
save("doc.short.sig", sig[:63])
PY
for variant in flip flipS mall short; do
    "$W" verify doc --pubkey "$(cat k.pub)" --sig "doc.$variant.sig" >/dev/null 2>&1 && fail "forged signature accepted: $variant"
done
cp doc doc2; printf 'x' >> doc2; cp doc.sig doc2.sig
"$W" verify doc2 --pubkey "$(cat k.pub)" >/dev/null 2>&1 && fail "a changed message verified"
"$W" verify doc --pubkey "$(cat k2.pub)" >/dev/null 2>&1 && fail "another key verified the signature"
"$W" verify doc --pubkey abcd >/dev/null 2>&1 && fail "a short public key was accepted"
printf '%064d\n' 0 > zero.pub
"$W" verify doc --pubkey "$(cat zero.pub)" >/dev/null 2>&1 && fail "the all-zero key verified a signature"
ok "forgeries are refused: flipped bits, S+L (non-canonical), truncated, changed message, wrong key, bad keys"

# ── against OpenSSL, both directions ──
if command -v openssl >/dev/null 2>&1 && openssl pkeyutl -help 2>&1 | grep -q rawin; then
    # (no empty message here: openssl pkeyutl cannot sign an empty file; RFC vector 1 covers it)
    for n in 1 63 64 65 1000 100000; do
        openssl genpkey -algorithm ed25519 -out o.pem 2>/dev/null
        openssl pkey -in o.pem -outform DER 2>/dev/null | tail -c 32 | od -An -tx1 | tr -d ' \n' > o.priv; echo >> o.priv
        pub="$(openssl pkey -in o.pem -pubout -outform DER 2>/dev/null | tail -c 32 | od -An -tx1 | tr -d ' \n')"
        [ "$("$W" pubkey o.priv)" = "$pub" ] || fail "public key differs from OpenSSL's"
        head -c "$n" /dev/urandom > m
        openssl pkeyutl -sign -rawin -inkey o.pem -in m -out m.osig 2>/dev/null || fail "openssl could not sign $n bytes (test environment)"
        python3 -c "import base64; print(base64.b64encode(open('m.osig','rb').read()).decode())" > m.sig
        "$W" verify m --pubkey "$pub" >/dev/null || fail "OpenSSL signature of $n bytes refused"
        "$W" sign m o.priv >/dev/null
        python3 -c "import base64,sys; sys.exit(0 if base64.b64decode(open('m.sig').read())==open('m.osig','rb').read() else 1)" || fail "signature of $n bytes differs from OpenSSL's"
        openssl pkey -in o.pem -pubout -out o.pub.pem 2>/dev/null
        openssl pkeyutl -verify -rawin -pubin -inkey o.pub.pem -in m -sigfile m.osig >/dev/null 2>&1 || fail "OpenSSL refused its own signature (test bug)"
    done
    ok "differential test with OpenSSL (6 sizes): same public keys, same signatures, accepted both ways"
else
    echo "skip: no openssl command with -rawin, differential test not run"
fi

echo "PASS: crypto"
