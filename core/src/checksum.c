/* FluxDM — core/src/checksum.c */

#include "../include/checksum.h"
#include "../include/errors.h"
#include <openssl/evp.h>
#include <openssl/err.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct flux_cksum_ctx {
    EVP_MD_CTX       *evp;
    flux_cksum_type_t type;
};

static const EVP_MD *evp_for_type(flux_cksum_type_t type) {
    switch (type) {
        case FLUX_CKSUM_MD5:    return EVP_md5();
        case FLUX_CKSUM_SHA256: return EVP_sha256();
        default:                return NULL;
    }
}

flux_cksum_ctx_t *flux_cksum_new(flux_cksum_type_t type) {
    if (type == FLUX_CKSUM_NONE) {
        flux_set_error(FLUX_ERR_INTERNAL, "cksum_new: NONE type");
        return NULL;
    }

    flux_cksum_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        flux_set_error(FLUX_ERR_NOMEM, "cksum_new: malloc");
        return NULL;
    }

    ctx->type = type;
    ctx->evp  = EVP_MD_CTX_new();
    if (!ctx->evp) {
        free(ctx);
        flux_set_error(FLUX_ERR_TLS_INIT, "EVP_MD_CTX_new failed");
        return NULL;
    }

    if (EVP_DigestInit_ex(ctx->evp, evp_for_type(type), NULL) != 1) {
        EVP_MD_CTX_free(ctx->evp);
        free(ctx);
        flux_set_error(FLUX_ERR_TLS_INIT, "EVP_DigestInit_ex failed");
        return NULL;
    }
    return ctx;
}

int flux_cksum_update(flux_cksum_ctx_t *ctx, const void *data, size_t len) {
    if (EVP_DigestUpdate(ctx->evp, data, len) != 1) {
        flux_set_error(FLUX_ERR_TLS_IO, "EVP_DigestUpdate failed");
        return -1;
    }
    return 0;
}

int flux_cksum_final(flux_cksum_ctx_t *ctx, uint8_t *out, size_t *out_len) {
    unsigned int dlen = 0;
    /* Copy ctx so it can be reused after finalisation */
    EVP_MD_CTX *copy = EVP_MD_CTX_new();
    if (!copy || EVP_MD_CTX_copy_ex(copy, ctx->evp) != 1) {
        flux_set_error(FLUX_ERR_TLS_IO, "EVP_MD_CTX_copy_ex failed");
        EVP_MD_CTX_free(copy);
        return -1;
    }

    if (EVP_DigestFinal_ex(copy, out, &dlen) != 1) {
        EVP_MD_CTX_free(copy);
        flux_set_error(FLUX_ERR_TLS_IO, "EVP_DigestFinal_ex failed");
        return -1;
    }
    EVP_MD_CTX_free(copy);

    /* Reset the original context so it can be reused immediately.
     * Without this, the next update() appends to the previous stream. */
    EVP_DigestInit_ex(ctx->evp, evp_for_type(ctx->type), NULL);

    if (out_len) *out_len = dlen;
    return 0;
}

int flux_cksum_verify(flux_cksum_ctx_t *ctx,
                      const uint8_t *expected, size_t expected_len) {
    uint8_t actual[FLUX_CKSUM_MAX_LEN];
    size_t  actual_len = 0;

    if (flux_cksum_final(ctx, actual, &actual_len) < 0) return -1;
    if (actual_len != expected_len) return 0;

    /* Constant-time comparison */
    unsigned char diff = 0;
    for (size_t i = 0; i < actual_len; i++)
        diff |= actual[i] ^ expected[i];
    return diff == 0 ? 1 : 0;
}

void flux_cksum_free(flux_cksum_ctx_t *ctx) {
    if (!ctx) return;
    EVP_MD_CTX_free(ctx->evp);
    free(ctx);
}

int flux_cksum_file(const char *path, flux_cksum_type_t type,
                    uint8_t *out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { flux_set_syserror("cksum_file: fopen"); return -1; }

    flux_cksum_ctx_t *ctx = flux_cksum_new(type);
    if (!ctx) { fclose(f); return -1; }

    uint8_t buf[65536];
    size_t  n;
    int     rc = 0;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (flux_cksum_update(ctx, buf, n) < 0) { rc = -1; break; }
    }
    if (rc == 0 && ferror(f)) {
        flux_set_syserror("cksum_file: fread");
        rc = -1;
    }

    if (rc == 0) rc = flux_cksum_final(ctx, out, out_len);

    flux_cksum_free(ctx);
    fclose(f);
    return rc;
}

void flux_cksum_hex(const uint8_t *digest, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[ digest[i]       & 0xF];
    }
    out[len * 2] = '\0';
}
