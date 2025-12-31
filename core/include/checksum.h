/* FluxDM — core/include/checksum.h
 * Streaming SHA-256 and MD5 checksum computation using OpenSSL EVP API.
 * The downloader feeds bytes as they arrive; verification runs at completion.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef enum {
    FLUX_CKSUM_NONE   = 0,
    FLUX_CKSUM_MD5    = 1,
    FLUX_CKSUM_SHA256 = 2,
} flux_cksum_type_t;

#define FLUX_CKSUM_MAX_LEN 32   /* SHA256 = 32 bytes, MD5 = 16 bytes */

/* Opaque context (wraps OpenSSL EVP_MD_CTX) */
typedef struct flux_cksum_ctx flux_cksum_ctx_t;

/* Allocate and initialise a new streaming checksum context.
 * Returns NULL on failure (flux error set). */
flux_cksum_ctx_t *flux_cksum_new(flux_cksum_type_t type);

/* Feed bytes into the running hash. Returns 0 on success, -1 on error. */
int  flux_cksum_update(flux_cksum_ctx_t *ctx, const void *data, size_t len);

/* Finalise and write the digest into out (must be FLUX_CKSUM_MAX_LEN bytes).
 * Sets *out_len to the actual digest length.
 * The context is reset and can be reused after this call. */
int  flux_cksum_final(flux_cksum_ctx_t *ctx, uint8_t *out, size_t *out_len);

/* Convenience: compare computed digest against expected bytes.
 * Returns 1 if matching, 0 if mismatch, -1 on error. */
int  flux_cksum_verify(flux_cksum_ctx_t *ctx,
                       const uint8_t *expected, size_t expected_len);

/* Free the context. */
void flux_cksum_free(flux_cksum_ctx_t *ctx);

/* One-shot file hash (reads entire file). Useful for post-download verify. */
int  flux_cksum_file(const char *path, flux_cksum_type_t type,
                     uint8_t *out, size_t *out_len);

/* Format a raw digest as a lowercase hex string (out must hold 2*len+1 bytes).*/
void flux_cksum_hex(const uint8_t *digest, size_t len, char *out);
