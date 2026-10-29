#include "psa/crypto.h"

#include "mbedtls/sha256.h"

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length,
                              uint8_t *hash, size_t hash_size, size_t *hash_length)
{
    if (alg != PSA_ALG_SHA_256 || hash == NULL || hash_size < 32 || hash_length == NULL) {
        return -1;
    }
    if (mbedtls_sha256(input, input_length, hash, 0) != 0) return -1;
    *hash_length = 32;
    return PSA_SUCCESS;
}
