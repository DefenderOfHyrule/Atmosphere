#include <stdint.h>
#include <string.h>

#include "utils.h"
#include "fuse.h"
#include "gcm.h"

#include "sealedkeys.h"
#include "se.h"

/* Shifts right a little endian 128-bit value. */
static void shr_128(uint64_t *val) {
    val[0] >>= 1;
    val[0] |= (val[1] & 1) << 63;
    val[1] >>= 1;
}

/* Shifts left a little endian 128-bit value. */
static void shl_128(uint64_t *val) {
    val[1] <<= 1;
    val[1] |= (val[0] & (1ull << 63)) >> 63;
    val[0] <<= 1;
}


/* Multiplies two 128-bit numbers X,Y in the GF(128) Galois Field. */
static void gf128_mul(uint8_t *dst, const uint8_t *x, const uint8_t *y) {
    uint8_t x_work[0x10];
    uint8_t y_work[0x10];
    uint8_t dst_work[0x10];

    uint64_t *p_x = (uint64_t *)(&x_work[0]);
    uint64_t *p_y = (uint64_t *)(&y_work[0]);
    uint64_t *p_dst = (uint64_t *)(&dst_work[0]);

    /* Initialize buffers. */
    for (unsigned int i = 0; i < 0x10; i++) {
        x_work[i] = x[0xF-i];
        y_work[i] = y[0xF-i];
        dst_work[i] = 0;
    }

    /* Perform operation for each bit in y. */
    for (unsigned int round = 0; round < 0x80; round++) {
        p_dst[0] ^= p_x[0] * ((y_work[0xF] & 0x80) >> 7);
        p_dst[1] ^= p_x[1] * ((y_work[0xF] & 0x80) >> 7);
        shl_128(p_y);
        uint8_t xval = 0xE1 * (x_work[0] & 1);
        shr_128(p_x);
        x_work[0xF] ^= xval;
    }

    for (unsigned int i = 0; i < 0x10; i++) {
        dst[i] = dst_work[0xF-i];
    }
}



/* Performs an AES-GCM GHASH operation over the data into dst. */
static void ghash(void *dst, const void *data, size_t data_size, const void *j_block, bool encrypt) {
    uint8_t x[0x10];
    uint8_t h[0x10];

    uint64_t *p_x = (uint64_t *)(&x[0]);
    uint64_t *p_data = (uint64_t *)data;

    memset(x, 0, 0x10);

    /* H = aes_ecb_encrypt(zeroes) */
    se_aes_128_ecb_encrypt_block(KEYSLOT_SWITCH_TEMPKEY, h, 0x10, x, 0x10);

    size_t total_size = data_size;

    while (data_size >= 0x10) {
        /* X = (X ^ current_block) * H */
        p_x[0] ^= p_data[0];
        p_x[1] ^= p_data[1];

        gf128_mul(x, x, h);

        /* Increment p_data by 0x10 bytes. */
        p_data += 2;
        data_size -= 0x10;
    }

    /* Nintendo's code *discards all data in the last block* if unaligned. */
    /* And treats that block as though it were all-zero. */
    /* This is a bug, they just forget to XOR with the copy of the last block they save. */
    if (data_size & 0xF) {
        gf128_mul(x, x, h);
    }

    /* Due to a Nintendo bug, the wrong QWORD gets XOR'd in the "final output block" case. */
    if (encrypt) {
        p_x[1] ^= (uint64_t)(total_size << 3);
    } else {
        p_x[0] ^= (uint64_t)(total_size << 3);
    }

    gf128_mul(x, x, h);

    /* If final output block, XOR with encrypted J block. */
    if (encrypt) {
        se_aes_128_ecb_encrypt_block(KEYSLOT_SWITCH_TEMPKEY, h, 0x10, j_block, 0x10);
        for (unsigned int i = 0; i < 0x10; i++) {
            x[i] ^= h[i];
        }
    }

    /* Copy output. */
    memcpy(dst, x, 0x10);
}


/* This function is a doozy. It decrypts and validates a (non-standard) AES-GCM wrapped keypair. */
size_t gcm_decrypt_key(void *dst, size_t dst_size, const void *src, size_t src_size, const void *sealed_kek, size_t kek_size, const void *wrapped_key, size_t key_size, unsigned int usecase, bool is_personalized) {
    if (is_personalized == 0) {
        /* Devkit keys use a different keyformat without a MAC/Device ID. */
        if (src_size <= 0x10 || src_size - 0x10 > dst_size) {
            generic_panic();
        }
    } else {
        if (src_size <= 0x30 || src_size - 0x20 > dst_size) {
            generic_panic();
        }
    }

    /* Unwrap the key */
    unseal_key(KEYSLOT_SWITCH_TEMPKEY, sealed_kek, kek_size, usecase);
    decrypt_data_into_keyslot(KEYSLOT_SWITCH_TEMPKEY, KEYSLOT_SWITCH_TEMPKEY, wrapped_key, key_size);

    /* Decrypt the GCM keypair, AES-CTR with CTR = blob[:0x10]. */
    se_aes_ctr_crypt(KEYSLOT_SWITCH_TEMPKEY, dst, dst_size, src + 0x10, src_size - 0x10, src, 0x10);


    if (!is_personalized) {
        /* Devkit non-personalized keys have no further authentication. */
        return src_size - 0x10;
    }

    /* J = GHASH(CTR); */
    uint8_t j_block[0x10];
    ghash(j_block, src, 0x10, NULL, false);

    /* MAC = GHASH(PLAINTEXT) ^ ENCRYPT(J) */
    /* Note: That MAC is calculated over plaintext is non-standard. */
    /* It is supposed to be over the ciphertext. */
    uint8_t calc_mac[0x10];
    ghash(calc_mac, dst, src_size - 0x20, j_block, true);

    /* Const-time memcmp. */
    const uint8_t *src_bytes = src;
    int different = 0;
    for (unsigned int i = 0; i < 0x10; i++) {
        different |= src_bytes[src_size - 0x10  + i] ^ calc_mac[i];
    }
    if (different) {
        return 0;
    }

    if (read64le(src_bytes, src_size - 0x28) != fuse_get_device_id()) {
        return 0;
    }

    return src_size - 0x30;
}
