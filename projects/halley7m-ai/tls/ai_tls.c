#include "ai_tls.h"

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "psa/crypto_extra.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct ai_tls_client {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    ai_tls_options_t io;
    int last_error;
    unsigned established;
    unsigned failed;
    unsigned io_failed;
    unsigned peer_closed;
    unsigned closing;
    unsigned local_closed;
    unsigned write_pending;
};

static ai_tls_platform_t platform_hooks;
static unsigned platform_ready;
static unsigned clients;

static int read_utc(mbedtls_time_t *result)
{
    int64_t seconds;
    if (platform_hooks.utc_seconds == NULL ||
        platform_hooks.utc_seconds(platform_hooks.context, &seconds) != 0 ||
        seconds < INT64_C(1767225600) || seconds >= INT64_C(4102444800)) {
        return AI_TLS_ERR_NOT_READY;
    }
    *result = (mbedtls_time_t) seconds;
    if ((int64_t) *result != seconds) {
        return AI_TLS_ERR_NOT_READY;
    }
    return AI_TLS_OK;
}

static mbedtls_time_t tls_time(mbedtls_time_t *result)
{
    mbedtls_time_t value = (mbedtls_time_t) -1;
    (void) read_utc(&value);
    if (result != NULL) {
        *result = value;
    }
    return value;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    if (platform_hooks.monotonic_ms == NULL) {
        return 0;
    }
    return (mbedtls_ms_time_t) platform_hooks.monotonic_ms(platform_hooks.context);
}

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
                                size_t *estimate_bits,
                                unsigned char *output, size_t output_size)
{
    size_t bits = 0;
    *estimate_bits = 0;
    if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    if (platform_hooks.entropy == NULL || output_size > SIZE_MAX / 8 ||
        platform_hooks.entropy(platform_hooks.context, output, output_size, &bits) != 0 ||
        bits != output_size * 8) {
        if (output_size != 0) {
            memset(output, 0, output_size);
        }
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *estimate_bits = bits;
    return 0;
}

int ai_tls_platform_init(const ai_tls_platform_t *platform)
{
    mbedtls_time_t now;
    if (platform_ready || clients != 0) {
        return AI_TLS_ERR_BUSY;
    }
    if (platform == NULL || platform->entropy == NULL ||
        platform->utc_seconds == NULL || platform->monotonic_ms == NULL) {
        return AI_TLS_ERR_NOT_READY;
    }
    platform_hooks = *platform;
    if (read_utc(&now) != AI_TLS_OK) {
        memset(&platform_hooks, 0, sizeof(platform_hooks));
        return AI_TLS_ERR_NOT_READY;
    }
    (void) mbedtls_platform_set_time(tls_time);
    if (psa_crypto_init() != PSA_SUCCESS) {
        mbedtls_psa_crypto_free();
        memset(&platform_hooks, 0, sizeof(platform_hooks));
        return AI_TLS_ERR_NOT_READY;
    }
    platform_ready = 1;
    return AI_TLS_OK;
}

int ai_tls_platform_shutdown(void)
{
    if (clients != 0) {
        return AI_TLS_ERR_BUSY;
    }
    if (platform_ready) {
        mbedtls_psa_crypto_free();
    }
    platform_ready = 0;
    memset(&platform_hooks, 0, sizeof(platform_hooks));
    return AI_TLS_OK;
}

int ai_tls_random(unsigned char *output, size_t length)
{
    if (output == NULL && length != 0) {
        return AI_TLS_ERR_ARGUMENT;
    }
    if (!platform_ready) {
        return AI_TLS_ERR_NOT_READY;
    }
    return psa_generate_random(output, length) == PSA_SUCCESS ?
           AI_TLS_OK : AI_TLS_ERR_NOT_READY;
}

static int send_bytes(void *context, const unsigned char *data, size_t length)
{
    ai_tls_client_t *client = context;
    int result = client->io.send(client->io.io_context, data, length);
    if (result == AI_TLS_IO_AGAIN) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (result <= 0 || (size_t) result > length) {
        client->io_failed = 1;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return result;
}

static int receive_bytes(void *context, unsigned char *data, size_t length)
{
    ai_tls_client_t *client = context;
    int result = client->io.receive(client->io.io_context, data, length);
    if (result == AI_TLS_IO_AGAIN) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (result < 0 || (size_t) result > length) {
        client->io_failed = 1;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return result;
}

static int tls_result(ai_tls_client_t *client, int result)
{
    if (result == 0) {
        return AI_TLS_OK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        return AI_TLS_WANT_READ;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return AI_TLS_WANT_WRITE;
    }
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        client->established = 0;
        client->peer_closed = 1;
        return AI_TLS_CLOSED;
    }
    client->last_error = result;
    client->failed = 1;
    if (client->io_failed || result == MBEDTLS_ERR_SSL_CONN_EOF) {
        return AI_TLS_ERR_IO;
    }
    if (result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ||
        (mbedtls_ssl_get_verify_result(&client->ssl) != 0 &&
         mbedtls_ssl_get_verify_result(&client->ssl) != UINT32_MAX)) {
        return AI_TLS_ERR_VERIFY;
    }
    return AI_TLS_ERR_TLS;
}

void ai_tls_destroy(ai_tls_client_t *client)
{
    if (client != NULL) {
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_config_free(&client->config);
        mbedtls_x509_crt_free(&client->ca);
        free(client);
        --clients;
    }
}

int ai_tls_create(ai_tls_client_t **result, const ai_tls_options_t *options)
{
    ai_tls_client_t *client;
    mbedtls_time_t now;
    int error;
    if (result == NULL) {
        return AI_TLS_ERR_ARGUMENT;
    }
    *result = NULL;
    if (options == NULL || options->hostname == NULL || options->hostname[0] == '\0' ||
        options->ca == NULL || options->ca_length == 0 ||
        options->send == NULL || options->receive == NULL) {
        return AI_TLS_ERR_ARGUMENT;
    }
    if (!platform_ready || read_utc(&now) != AI_TLS_OK) {
        return AI_TLS_ERR_NOT_READY;
    }
    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return AI_TLS_ERR_MEMORY;
    }
    ++clients;
    client->io = *options;
    mbedtls_ssl_init(&client->ssl);
    mbedtls_ssl_config_init(&client->config);
    mbedtls_x509_crt_init(&client->ca);
    error = mbedtls_x509_crt_parse(&client->ca, options->ca, options->ca_length);
    if (error == 0) {
        error = mbedtls_ssl_config_defaults(&client->config, MBEDTLS_SSL_IS_CLIENT,
                                            MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (error == 0) {
        mbedtls_ssl_conf_authmode(&client->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&client->config, &client->ca, NULL);
        error = mbedtls_ssl_setup(&client->ssl, &client->config);
    }
    if (error == 0) {
        error = mbedtls_ssl_set_hostname(&client->ssl, options->hostname);
    }
    if (error != 0) {
        ai_tls_destroy(client);
        return AI_TLS_ERR_TLS;
    }
    mbedtls_ssl_set_bio(&client->ssl, client, send_bytes, receive_bytes, NULL);
    *result = client;
    return AI_TLS_OK;
}

int ai_tls_handshake(ai_tls_client_t *client)
{
    mbedtls_time_t now;
    int result;
    if (client == NULL) {
        return AI_TLS_ERR_ARGUMENT;
    }
    if (client->failed || client->closing || client->peer_closed ||
        !platform_ready || read_utc(&now) != AI_TLS_OK) {
        return AI_TLS_ERR_NOT_READY;
    }
    result = tls_result(client, mbedtls_ssl_handshake(&client->ssl));
    if (result == AI_TLS_OK) {
        if (mbedtls_ssl_get_verify_result(&client->ssl) != 0) {
            client->failed = 1;
            return AI_TLS_ERR_VERIFY;
        }
        client->established = 1;
    }
    return result;
}

int ai_tls_read(ai_tls_client_t *client, unsigned char *data, size_t capacity,
                size_t *received)
{
    int result;
    if (received == NULL || client == NULL || data == NULL || capacity == 0 || capacity > INT_MAX) {
        return AI_TLS_ERR_ARGUMENT;
    }
    *received = 0;
    if (client->peer_closed) {
        return AI_TLS_CLOSED;
    }
    if (!client->established || client->failed || client->closing) {
        return AI_TLS_ERR_NOT_READY;
    }
    result = mbedtls_ssl_read(&client->ssl, data, capacity);
    if (result > 0) {
        *received = (size_t) result;
        return AI_TLS_OK;
    }
    if (result == 0) {
        /* A transport EOF without close_notify is a truncated TLS stream. */
        client->failed = 1;
        client->last_error = MBEDTLS_ERR_SSL_CONN_EOF;
        return AI_TLS_ERR_IO;
    }
    return tls_result(client, result);
}

int ai_tls_write(ai_tls_client_t *client, const unsigned char *data, size_t length,
                 size_t *written)
{
    int result;
    if (written == NULL || client == NULL || data == NULL || length == 0 || length > INT_MAX) {
        return AI_TLS_ERR_ARGUMENT;
    }
    *written = 0;
    if (!client->established || client->failed || client->closing) {
        return AI_TLS_ERR_NOT_READY;
    }
    result = mbedtls_ssl_write(&client->ssl, data, length);
    client->write_pending = result == MBEDTLS_ERR_SSL_WANT_READ ||
                            result == MBEDTLS_ERR_SSL_WANT_WRITE;
    if (result > 0) {
        *written = (size_t) result;
        return AI_TLS_OK;
    }
    return tls_result(client, result);
}

int ai_tls_close_notify(ai_tls_client_t *client)
{
    int result;
    if (client == NULL) {
        return AI_TLS_ERR_ARGUMENT;
    }
    if (client->write_pending) {
        return AI_TLS_ERR_BUSY;
    }
    if ((!client->established && !client->peer_closed) || client->failed || client->local_closed) {
        return AI_TLS_ERR_NOT_READY;
    }
    client->closing = 1;
    result = tls_result(client, mbedtls_ssl_close_notify(&client->ssl));
    if (result == AI_TLS_OK) {
        client->established = 0;
        client->local_closed = 1;
    }
    return result;
}

int ai_tls_last_error(const ai_tls_client_t *client)
{
    return client != NULL ? client->last_error : 0;
}

uint32_t ai_tls_verify_flags(const ai_tls_client_t *client)
{
    return client != NULL ? mbedtls_ssl_get_verify_result(&client->ssl) : UINT32_MAX;
}
