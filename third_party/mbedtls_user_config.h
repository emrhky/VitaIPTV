/* mbedTLS settings for VitaIPTV (added after the default mbedtls_config.h).
 * The Vita is not "unix" for mbedTLS: no platform entropy, sockets or timers from the library;
 * src/curlio.c brings its own (newlib's getentropy, BSD sockets and poll). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT          /* mbedtls_hardware_poll() in src/curlio.c */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C           /* no key files */
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_FS_IO
#define MBEDTLS_THREADING_C                   /* playlists (main thread) and the player can use TLS at once */
#define MBEDTLS_THREADING_PTHREAD
#define MBEDTLS_PLATFORM_MS_TIME_ALT          /* mbedtls_ms_time() in src/curlio.c (the Vita is not "unix") */
