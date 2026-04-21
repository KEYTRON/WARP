#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include "warp.h"

/* ── master public key (placeholder — replace with real key) ─── */
/* Generate with: warp keygen                                      */
static const uint8_t WARP_MASTER_PUBKEY[32] = {
    /* TODO: replace with real Ed25519 public key after warp keygen */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
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

/* ── base64 decode (simple, no padding variants) ─────────────── */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int warp_base64_decode(const char *in, uint8_t *out, size_t *out_len) {
    size_t in_len = strlen(in);
    if (in_len % 4 != 0) return WARP_ERR_INVAL;
    *out_len = in_len / 4 * 3;
    if (in[in_len - 1] == '=') (*out_len)--;
    if (in[in_len - 2] == '=') (*out_len)--;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        uint32_t a = (uint32_t)(strchr(b64_table, in[i])   - b64_table);
        uint32_t b = (uint32_t)(strchr(b64_table, in[i+1]) - b64_table);
        uint32_t c = in[i+2] == '=' ? 0 : (uint32_t)(strchr(b64_table, in[i+2]) - b64_table);
        uint32_t d = in[i+3] == '=' ? 0 : (uint32_t)(strchr(b64_table, in[i+3]) - b64_table);
        uint32_t triple = (a << 18) | (b << 12) | (c << 6) | d;
        if (j < *out_len) out[j++] = (triple >> 16) & 0xFF;
        if (j < *out_len) out[j++] = (triple >>  8) & 0xFF;
        if (j < *out_len) out[j++] =  triple        & 0xFF;
    }
    return WARP_OK;
}

int warp_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((in_len + 2) / 3) * 4 + 1;
    if (out_cap < need) return WARP_ERR_INVAL;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = table[(triple >> 18) & 0x3f];
        out[j++] = table[(triple >> 12) & 0x3f];
        out[j++] = (i + 1 < in_len) ? table[(triple >> 6) & 0x3f] : '=';
        out[j++] = (i + 2 < in_len) ? table[triple & 0x3f] : '=';
    }
    out[j] = '\0';
    return WARP_OK;
}

int warp_hex_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t len = strlen(in);
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

/* ── Ed25519 verify ───────────────────────────────────────────── */
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

int warp_sign_buf(const uint8_t *msg, size_t msg_len, const uint8_t *privkey, size_t privkey_len,
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

/* ── verify index signature ───────────────────────────────────── */
int warp_verify_index_sig(const char *index_json, const char *sig_b64) {
#ifdef WARP_SKIP_SIG_VERIFY
    (void)index_json; (void)sig_b64;
    return WARP_OK;
#else
    /* Check if master key is initialised */
    int zeroes = 1;
    for (int i = 0; i < 32; i++) if (WARP_MASTER_PUBKEY[i]) { zeroes = 0; break; }
    if (zeroes) {
        warp_warn("Master public key not set — skipping signature verification");
        return WARP_OK;
    }

    uint8_t sig[64];
    size_t sig_len = 0;
    if (warp_base64_decode(sig_b64, sig, &sig_len) != WARP_OK || sig_len != 64)
        return WARP_ERR_SIG;

    return warp_ed25519_verify(
        (const uint8_t *)index_json, strlen(index_json),
        sig, WARP_MASTER_PUBKEY);
#endif
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
