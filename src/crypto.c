#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include "warp.h"

/* ── master public key ──────────────────────────────────────────
 * Ed25519 public key for the KEYTRON/WARP release signing key.
 * Generated with: warp keygen
 * The matching private key never lives in this repo — it is kept
 * out-of-tree and used only by `warp sign` when publishing an index. */
const uint8_t WARP_MASTER_PUBKEY[32] = {
    0xe3,0xda,0x01,0x79,0xff,0xb4,0x0f,0xfc,
    0xe2,0x01,0x77,0xe9,0x09,0x55,0x43,0xd8,
    0x43,0x51,0x88,0x2d,0x93,0x0b,0x3e,0x2f,
    0xe1,0xbd,0xe0,0x1c,0x36,0x33,0xbc,0xc7,
};

/* ── SHA256 of a file ─────────────────────────────────────────── */
int warp_sha256_file(const char *path, char out_hex[WARP_SHA256_HEX]) {
    FILE *f = fopen(path, "rb");
    if (!f) return WARP_ERR_IO;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { fclose(f); return WARP_ERR_IO; }

    int rc = WARP_OK;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        rc = WARP_ERR_HASH; goto done;
    }

    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            rc = WARP_ERR_HASH; goto done;
        }
    }
    if (ferror(f)) { rc = WARP_ERR_IO; goto done; }

    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        rc = WARP_ERR_HASH; goto done;
    }

    for (unsigned int i = 0; i < digest_len; i++)
        snprintf(out_hex + i*2, 3, "%02x", digest[i]);
    out_hex[digest_len * 2] = '\0';

done:
    EVP_MD_CTX_free(ctx);
    fclose(f);
    return rc;
}

/* ── SHA256 of a buffer ───────────────────────────────────────── */
int warp_sha256_buf(const uint8_t *buf, size_t len, char out_hex[WARP_SHA256_HEX]) {
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!EVP_Digest(buf, len, digest, &digest_len, EVP_sha256(), NULL))
        return WARP_ERR_HASH;
    for (unsigned int i = 0; i < digest_len; i++)
        snprintf(out_hex + i*2, 3, "%02x", digest[i]);
    out_hex[digest_len * 2] = '\0';
    return WARP_OK;
}

/* ── base64 decode (simple, no padding variants; tolerates
 *    surrounding/embedded whitespace so a `\n`-terminated .sig
 *    file read straight off disk still decodes) ─────────────── */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int warp_base64_decode(const char *in, uint8_t *out, size_t *out_len) {
    char clean[256];
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < sizeof(clean); p++) {
        if (!isspace((unsigned char)*p)) clean[n++] = *p;
    }
    clean[n] = '\0';

    size_t in_len = strlen(clean);
    if (in_len == 0 || in_len % 4 != 0) return WARP_ERR_INVAL;
    *out_len = in_len / 4 * 3;
    if (clean[in_len - 1] == '=') (*out_len)--;
    if (clean[in_len - 2] == '=') (*out_len)--;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        const char *pa = strchr(b64_table, clean[i]);
        const char *pb = strchr(b64_table, clean[i+1]);
        if (!pa || !pb) return WARP_ERR_INVAL;
        uint32_t a = (uint32_t)(pa - b64_table);
        uint32_t b = (uint32_t)(pb - b64_table);
        uint32_t c = 0, d = 0;
        if (clean[i+2] != '=') {
            const char *pc = strchr(b64_table, clean[i+2]);
            if (!pc) return WARP_ERR_INVAL;
            c = (uint32_t)(pc - b64_table);
        }
        if (clean[i+3] != '=') {
            const char *pd = strchr(b64_table, clean[i+3]);
            if (!pd) return WARP_ERR_INVAL;
            d = (uint32_t)(pd - b64_table);
        }
        uint32_t triple = (a << 18) | (b << 12) | (c << 6) | d;
        if (j < *out_len) out[j++] = (triple >> 16) & 0xFF;
        if (j < *out_len) out[j++] = (triple >>  8) & 0xFF;
        if (j < *out_len) out[j++] =  triple        & 0xFF;
    }
    return WARP_OK;
}

int warp_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    size_t need = ((in_len + 2) / 3) * 4 + 1;
    if (out_cap < need) return WARP_ERR_INVAL;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_table[(triple >> 18) & 0x3f];
        out[j++] = b64_table[(triple >> 12) & 0x3f];
        out[j++] = (i + 1 < in_len) ? b64_table[(triple >> 6) & 0x3f] : '=';
        out[j++] = (i + 2 < in_len) ? b64_table[triple & 0x3f]        : '=';
    }
    out[j] = '\0';
    return WARP_OK;
}

int warp_hex_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1])) len--;
    if (len % 2 != 0) return WARP_ERR_INVAL;
    size_t need = len / 2;
    if (out_cap < need) return WARP_ERR_INVAL;

    for (size_t i = 0; i < need; i++) {
        char hi = in[i * 2];
        char lo = in[i * 2 + 1];
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
            return WARP_ERR_INVAL;
        char tmp[3] = { hi, lo, '\0' };
        out[i] = (uint8_t)strtoul(tmp, NULL, 16);
    }
    if (out_len) *out_len = need;
    return WARP_OK;
}

/* ── Ed25519 sign / verify ───────────────────────────────────── */
int warp_ed25519_verify(const uint8_t *msg, size_t msg_len,
                         const uint8_t sig[64],
                         const uint8_t pubkey[32]) {
#ifdef WARP_SKIP_SIG_VERIFY
    (void)msg; (void)msg_len; (void)sig; (void)pubkey;
    return WARP_OK;
#else
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, NULL, pubkey, 32);
    if (!pkey) return WARP_ERR_SIG;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pkey); return WARP_ERR_SIG; }

    int rc = WARP_ERR_SIG;
    if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestVerify(ctx, sig, 64, msg, msg_len) == 1) {
        rc = WARP_OK;
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
#endif
}

int warp_sign_buf(const uint8_t *msg, size_t msg_len,
                   const uint8_t *privkey, size_t privkey_len,
                   uint8_t sig[64]) {
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, privkey, privkey_len);
    if (!pkey) return WARP_ERR_SIG;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return WARP_ERR_SIG;
    }

    size_t sig_len = 64;
    int rc = WARP_ERR_SIG;
    if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestSign(ctx, sig, &sig_len, msg, msg_len) == 1 &&
        sig_len == 64) {
        rc = WARP_OK;
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

int warp_sign_file(const char *path, const char *privkey_hex_path, char out_b64[128]) {
    size_t key_len = 0;
    char *key_hex = read_file(privkey_hex_path, &key_len);
    if (!key_hex) return WARP_ERR_IO;

    uint8_t priv[64] = {0};
    size_t priv_len = 0;
    int rc = warp_hex_decode(key_hex, priv, sizeof(priv), &priv_len);
    free(key_hex);
    if (rc != WARP_OK) return rc;

    size_t msg_len = 0;
    char *msg = read_file(path, &msg_len);
    if (!msg) return WARP_ERR_IO;

    uint8_t sig[64];
    rc = warp_sign_buf((const uint8_t *)msg, msg_len, priv, priv_len, sig);
    free(msg);
    if (rc != WARP_OK) return rc;

    return warp_base64_encode(sig, sizeof(sig), out_b64, 128);
}

/* ── verify a signature against the embedded master public key ──
 * Used for detached signatures (e.g. index.json + index.json.sig):
 * `data`/`data_len` is the exact byte range that was signed. */
int warp_verify_sig_with_key(const char *data, const char *sig_b64, const uint8_t pubkey[32]) {
#ifdef WARP_SKIP_SIG_VERIFY
    (void)data; (void)sig_b64; (void)pubkey;
    warp_warn("Signature verification disabled at build time (WARP_SKIP_SIG_VERIFY) — do not ship this build");
    return WARP_OK;
#else
    /* A zeroed key means no real trust root. That must never be silently
     * treated as "verification passed" — a prior regression did exactly
     * that and shipped an unsigned index for months. Fail closed. */
    int zeroes = 1;
    for (int i = 0; i < 32; i++) if (pubkey[i]) { zeroes = 0; break; }
    if (zeroes) {
        warp_err("No public key for this repository — refusing to verify its index");
        warp_err("(builds may define WARP_SKIP_SIG_VERIFY for local testing only)");
        return WARP_ERR_SIG;
    }

    if (!sig_b64 || !*sig_b64) return WARP_ERR_SIG;

    uint8_t sig[64];
    size_t sig_len = 0;
    if (warp_base64_decode(sig_b64, sig, &sig_len) != WARP_OK || sig_len != 64)
        return WARP_ERR_SIG;

    return warp_ed25519_verify((const uint8_t *)data, strlen(data), sig, pubkey);
#endif
}

int warp_verify_index_sig(const char *data, const char *sig_b64) {
    return warp_verify_sig_with_key(data, sig_b64, WARP_MASTER_PUBKEY);
}

/* ── keygen: generate Ed25519 keypair ────────────────────────── */
int warp_keygen(const char *privkey_path, const char *pubkey_path) {
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!kctx) return WARP_ERR_IO;

    EVP_PKEY *pkey = NULL;
    int rc = WARP_ERR_IO;

    if (EVP_PKEY_keygen_init(kctx) != 1) goto done;
    if (EVP_PKEY_keygen(kctx, &pkey) != 1) goto done;

    /* Export raw keys */
    uint8_t priv[64], pub[32];
    size_t priv_len = 64, pub_len = 32;
    if (EVP_PKEY_get_raw_private_key(pkey, priv, &priv_len) != 1) goto done;
    if (EVP_PKEY_get_raw_public_key (pkey, pub,  &pub_len)  != 1) goto done;

    /* Write hex files */
    FILE *fp = fopen(privkey_path, "w");
    if (!fp) goto done;
    for (size_t i = 0; i < priv_len; i++) fprintf(fp, "%02x", priv[i]);
    fprintf(fp, "\n");
    fclose(fp);

    fp = fopen(pubkey_path, "w");
    if (!fp) goto done;
    for (size_t i = 0; i < pub_len; i++) fprintf(fp, "%02x", pub[i]);
    fprintf(fp, "\n");
    fclose(fp);

    /* Print C array for embedding */
    printf("/* Paste into crypto.c WARP_MASTER_PUBKEY: */\n");
    printf("static const uint8_t WARP_MASTER_PUBKEY[32] = {\n    ");
    for (size_t i = 0; i < pub_len; i++) {
        printf("0x%02x", pub[i]);
        if (i < pub_len - 1) printf(",");
        if ((i + 1) % 8 == 0 && i < pub_len - 1) printf("\n    ");
    }
    printf("\n};\n");

    rc = WARP_OK;
done:
    if (pkey) EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(kctx);
    return rc;
}
