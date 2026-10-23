#ifndef AI_TLS_H
#define AI_TLS_H

#include <stddef.h>
#include <stdint.h>

typedef struct ai_tls_client ai_tls_client_t;

enum {
    AI_TLS_OK = 0,
    AI_TLS_WANT_READ = 1,
    AI_TLS_WANT_WRITE = 2,
    AI_TLS_CLOSED = 3,
    AI_TLS_ERR_ARGUMENT = -1,
    AI_TLS_ERR_NOT_READY = -2,
    AI_TLS_ERR_MEMORY = -3,
    AI_TLS_ERR_TLS = -4,
    AI_TLS_ERR_VERIFY = -5,
    AI_TLS_ERR_IO = -6,
    AI_TLS_ERR_BUSY = -7
};

/* BIO returns a positive byte count, zero for EOF, or one of these errors. */
enum { AI_TLS_IO_ERROR = -1, AI_TLS_IO_AGAIN = -2 };

typedef struct {
    void *context;
    /* Return 0 and fill every byte; report the measured/justified entropy bits.
     * The pinned crypto library requires 8 bits/byte. A changing raw DTRNG
     * word or a timer does not establish that property. Never inflate it. */
    int (*entropy)(void *context, unsigned char *output, size_t length,
                   size_t *entropy_bits);
    /* Return 0 only for usable, fresh UTC. Adapter accepts 2026..2099. */
    int (*utc_seconds)(void *context, int64_t *seconds);
    uint64_t (*monotonic_ms)(void *context);
} ai_tls_platform_t;

typedef struct {
    const char *hostname;
    /* PEM length includes the terminating NUL; DER length is exact. */
    const unsigned char *ca;
    size_t ca_length;
    void *io_context;
    int (*send)(void *context, const unsigned char *data, size_t length);
    int (*receive)(void *context, unsigned char *data, size_t length);
} ai_tls_options_t;

/* All functions and PSA calls belong to ONE network task, including global
 * init/shutdown. No concurrent use; no connection retry or socket ownership. */
int ai_tls_platform_init(const ai_tls_platform_t *platform);
int ai_tls_platform_shutdown(void);
int ai_tls_random(unsigned char *output, size_t length);
int ai_tls_create(ai_tls_client_t **client, const ai_tls_options_t *options);
int ai_tls_handshake(ai_tls_client_t *client);
int ai_tls_read(ai_tls_client_t *client, unsigned char *data, size_t capacity,
                size_t *received);
/* Retry WANT_* with the SAME data and length. On OK advance by *written. */
int ai_tls_write(ai_tls_client_t *client, const unsigned char *data, size_t length,
                 size_t *written);
/* Returns BUSY while a prior application write needs retry. Once closing
 * starts, only close_notify retries or destroy are permitted. */
int ai_tls_close_notify(ai_tls_client_t *client);
void ai_tls_destroy(ai_tls_client_t *client);
int ai_tls_last_error(const ai_tls_client_t *client);
uint32_t ai_tls_verify_flags(const ai_tls_client_t *client);

#endif
