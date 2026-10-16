// pspdev's mbedTLS is built with everything on, and its own code refers to all of it, so the linker would
// put in the TLS server, DES, Camellia, Blowfish, RC4, CCM, RIPEMD-160, DHE, encrypted key files, the
// debug printer and 50 KB of mbedTLS's test certificates (for its self-tests): about 130 KB the updater
// never runs. Defining their entry points here keeps those parts of the library out.
//
// None of these can be reached: update.c is a TLS 1.2 client that offers only ECDHE with AES-GCM or
// ChaCha20 (tls_init), doesn't call the self-tests and doesn't read key files. If a newer mbedTLS refers
// to something else in those files, the link fails with "multiple definition": then drop the stub here.
// No mbedTLS headers: the stubs only need the names.

#include <stddef.h>

#define STUB(name)      \
    int name(void);     \
    int name(void) {    \
        return -0x0070; \
    } // MBEDTLS_ERR_PLATFORM_FEATURE_UNSUPPORTED

// The TLS server side (ssl_srv.c)
STUB(mbedtls_ssl_handshake_server_step)

// Debug messages (debug.c): mbedTLS calls these everywhere, and they print only with a debug callback set
STUB(mbedtls_debug_print_msg)
STUB(mbedtls_debug_print_ret)
STUB(mbedtls_debug_print_buf)
STUB(mbedtls_debug_print_mpi)
STUB(mbedtls_debug_print_crt)
STUB(mbedtls_debug_printf_ecdh)

// Ciphers the client never offers (des.c, camellia.c, blowfish.c, arc4.c, ccm.c)
STUB(mbedtls_des_init)
STUB(mbedtls_des_free)
STUB(mbedtls_des_setkey_enc)
STUB(mbedtls_des_setkey_dec)
STUB(mbedtls_des_crypt_ecb)
STUB(mbedtls_des_crypt_cbc)
STUB(mbedtls_des_key_set_parity)
STUB(mbedtls_des3_init)
STUB(mbedtls_des3_free)
STUB(mbedtls_des3_set2key_enc)
STUB(mbedtls_des3_set2key_dec)
STUB(mbedtls_des3_set3key_enc)
STUB(mbedtls_des3_set3key_dec)
STUB(mbedtls_des3_crypt_ecb)
STUB(mbedtls_des3_crypt_cbc)
STUB(mbedtls_camellia_init)
STUB(mbedtls_camellia_free)
STUB(mbedtls_camellia_setkey_enc)
STUB(mbedtls_camellia_setkey_dec)
STUB(mbedtls_camellia_crypt_ecb)
STUB(mbedtls_camellia_crypt_cbc)
STUB(mbedtls_camellia_crypt_cfb128)
STUB(mbedtls_camellia_crypt_ctr)
STUB(mbedtls_blowfish_init)
STUB(mbedtls_blowfish_free)
STUB(mbedtls_blowfish_setkey)
STUB(mbedtls_blowfish_crypt_ecb)
STUB(mbedtls_blowfish_crypt_cbc)
STUB(mbedtls_blowfish_crypt_cfb64)
STUB(mbedtls_blowfish_crypt_ctr)
STUB(mbedtls_arc4_init)
STUB(mbedtls_arc4_free)
STUB(mbedtls_arc4_setup)
STUB(mbedtls_arc4_crypt)
STUB(mbedtls_ccm_init)
STUB(mbedtls_ccm_free)
STUB(mbedtls_ccm_setkey)
STUB(mbedtls_ccm_encrypt_and_tag)
STUB(mbedtls_ccm_auth_decrypt)

// Finite-field Diffie-Hellman (dhm.c): only for DHE cipher suites
STUB(mbedtls_dhm_init)
STUB(mbedtls_dhm_free)
STUB(mbedtls_dhm_read_params)
STUB(mbedtls_dhm_make_public)
STUB(mbedtls_dhm_calc_secret)

// RIPEMD-160 (ripemd160.c): no certificate GitHub uses is signed with it
STUB(mbedtls_ripemd160_init)
STUB(mbedtls_ripemd160_free)
STUB(mbedtls_ripemd160_clone)
STUB(mbedtls_ripemd160_starts_ret)
STUB(mbedtls_ripemd160_update_ret)
STUB(mbedtls_ripemd160_finish_ret)
STUB(mbedtls_ripemd160_ret)
STUB(mbedtls_internal_ripemd160_process)

// Password-protected key files (pkcs12.c, pkcs5.c)
STUB(mbedtls_pkcs12_pbe)
STUB(mbedtls_pkcs12_pbe_sha1_rc4_128)
STUB(mbedtls_pkcs5_pbes2)

// The test certificates (certs.c), for x509.c's self-test. mbedTLS was compiled to reach them through the
// global pointer, so they go in small data (the game itself builds with -G0).
#define SMALL __attribute__((section(".sdata")))
SMALL const char *mbedtls_test_ca_crt = "";
SMALL const size_t mbedtls_test_ca_crt_len = 0;
SMALL const char *mbedtls_test_cli_crt = "";
SMALL const size_t mbedtls_test_cli_crt_len = 0;
