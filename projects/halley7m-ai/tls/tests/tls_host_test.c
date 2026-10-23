/* Loopback-only integration test. Windows RNG belongs to this host harness. */
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai_tls.h"

typedef struct {
    int entropy_mode;
    unsigned entropy_calls;
    int64_t utc;
    SOCKET socket;
    unsigned send_calls;
    unsigned receive_calls;
} test_context_t;

static int entropy(void *opaque, unsigned char *output, size_t length, size_t *bits)
{
    test_context_t *context = opaque;
    ++context->entropy_calls;
    *bits = 0;
    if (context->entropy_mode == 0 || length > ULONG_MAX) {
        return -1;
    }
    if (BCryptGenRandom(NULL, output, (ULONG) length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return -1;
    }
    *bits = context->entropy_mode == 1 ? length : length * 8;
    return 0;
}

static int utc_seconds(void *opaque, int64_t *seconds)
{
    test_context_t *context = opaque;
    *seconds = context->utc;
    return *seconds != 0 ? 0 : -1;
}

static uint64_t monotonic_ms(void *opaque)
{
    (void) opaque;
    return GetTickCount64();
}

static int send_bytes(void *opaque, const unsigned char *data, size_t length)
{
    test_context_t *context = opaque;
    int result;
    /* Exercise partial records and WANT_WRITE without opening any remote host. */
    if (++context->send_calls % 3 == 0) {
        return AI_TLS_IO_AGAIN;
    }
    if (length > 37) {
        length = 37;
    }
    result = send(context->socket, (const char *) data, (int) length, 0);
    return result == SOCKET_ERROR ? AI_TLS_IO_ERROR : result;
}

static int receive_bytes(void *opaque, unsigned char *data, size_t length)
{
    test_context_t *context = opaque;
    int result;
    if (++context->receive_calls % 4 == 0) {
        return AI_TLS_IO_AGAIN;
    }
    if (length > 53) {
        length = 53;
    }
    result = recv(context->socket, (char *) data, (int) length, 0);
    return result == SOCKET_ERROR ? AI_TLS_IO_ERROR : result;
}

static unsigned char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    long size;
    unsigned char *data;
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0);
    rewind(file);
    data = malloc((size_t) size + 1);
    assert(data != NULL);
    assert(fread(data, 1, (size_t) size, file) == (size_t) size);
    fclose(file);
    data[size] = 0;
    *length = (size_t) size + 1;
    return data;
}

int main(int argc, char **argv)
{
    test_context_t context = {0};
    ai_tls_platform_t platform = {&context, entropy, utc_seconds, monotonic_ms};
    ai_tls_options_t options = {0};
    ai_tls_client_t *client = NULL;
    unsigned char random[32], random2[32];
    unsigned char *ca;
    size_t ca_length, count;
    int result;
    unsigned calls;
    assert(argc == 2 || argc == 7);
    ca = read_file(argv[1], &ca_length);
    assert(ai_tls_random(random, sizeof(random)) == AI_TLS_ERR_NOT_READY);
    assert(ai_tls_platform_init(NULL) == AI_TLS_ERR_NOT_READY);
    context.utc = 0;
    assert(ai_tls_platform_init(&platform) == AI_TLS_ERR_NOT_READY);
    assert(context.entropy_calls == 0);
    context.utc = INT64_C(1792713600); /* 2026-10-23, inside fixture validity. */
    assert(ai_tls_platform_init(&platform) == AI_TLS_ERR_NOT_READY);
    assert(context.entropy_calls > 0);
    context.entropy_mode = 1; /* Bytes read do not imply entropy bits. */
    assert(ai_tls_platform_init(&platform) == AI_TLS_ERR_NOT_READY);
    context.entropy_mode = 2;
    assert(ai_tls_platform_init(&platform) == AI_TLS_OK);
    assert(ai_tls_platform_init(&platform) == AI_TLS_ERR_BUSY);
    assert(ai_tls_random(random, sizeof(random)) == AI_TLS_OK);
    assert(ai_tls_random(random2, sizeof(random2)) == AI_TLS_OK);
    assert(memcmp(random, random2, sizeof(random)) != 0);
    options.hostname = "localhost";
    options.ca = ca;
    options.ca_length = ca_length;
    options.io_context = &context;
    options.send = send_bytes;
    options.receive = receive_bytes;
    assert(ai_tls_create(&client, &options) == AI_TLS_OK);
    assert(ai_tls_platform_shutdown() == AI_TLS_ERR_BUSY);
    assert(ai_tls_read(client, random, sizeof(random), &count) == AI_TLS_ERR_NOT_READY);
    context.utc = 0;
    assert(ai_tls_handshake(client) == AI_TLS_ERR_NOT_READY);
    assert(context.send_calls == 0 && context.receive_calls == 0);
    ai_tls_destroy(client);
    client = NULL;
    context.utc = INT64_C(1792713600);
    options.ca = (const unsigned char *) "invalid certificate";
    options.ca_length = sizeof("invalid certificate");
    assert(ai_tls_create(&client, &options) == AI_TLS_ERR_TLS && client == NULL);
    options.ca = ca;
    options.ca_length = ca_length;
    options.hostname = "";
    assert(ai_tls_create(&client, &options) == AI_TLS_ERR_ARGUMENT);
    if (argc == 7) {
        WSADATA data;
        struct sockaddr_in address = {0};
        DWORD timeout_ms = 5000;
        int expected = atoi(argv[5]);
        uint32_t expected_flags = (uint32_t) strtoul(argv[6], NULL, 0);
        assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
        context.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(context.socket != INVALID_SOCKET);
        assert(setsockopt(context.socket, SOL_SOCKET, SO_RCVTIMEO,
                          (const char *) &timeout_ms, sizeof(timeout_ms)) == 0);
        assert(setsockopt(context.socket, SOL_SOCKET, SO_SNDTIMEO,
                          (const char *) &timeout_ms, sizeof(timeout_ms)) == 0);
        address.sin_family = AF_INET;
        address.sin_port = htons((unsigned short) atoi(argv[2]));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        result = connect(context.socket, (const struct sockaddr *) &address, sizeof(address));
        if (result != 0) {
            fprintf(stderr, "loopback connect failed: WSA=%d port=%d\n", WSAGetLastError(), atoi(argv[2]));
        }
        assert(result == 0);
        options.hostname = argv[3];
        context.utc = _strtoi64(argv[4], NULL, 10);
        assert(ai_tls_create(&client, &options) == AI_TLS_OK);
        for (calls = 0; calls < 10000; ++calls) {
            result = ai_tls_handshake(client);
            if (result != AI_TLS_WANT_READ && result != AI_TLS_WANT_WRITE) {
                break;
            }
        }
        if (result != expected) {
            fprintf(stderr, "TLS got %d expected %d native=%d flags=%08lx\n",
                    result, expected, ai_tls_last_error(client),
                    (unsigned long) ai_tls_verify_flags(client));
        }
        assert(result == expected);
        if (expected_flags != 0) {
            assert((ai_tls_verify_flags(client) & expected_flags) == expected_flags);
        }
        if (expected == AI_TLS_OK) {
            static const unsigned char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
            unsigned char response[128] = {0};
            size_t sent = 0, received = 0;
            assert(ai_tls_verify_flags(client) == 0);
            for (calls = 0; sent < sizeof(request) - 1 && calls < 10000; ++calls) {
                result = ai_tls_write(client, request + sent, sizeof(request) - 1 - sent, &count);
                assert(result == AI_TLS_OK || result == AI_TLS_WANT_READ || result == AI_TLS_WANT_WRITE);
                if (result == AI_TLS_WANT_READ || result == AI_TLS_WANT_WRITE) {
                    assert(ai_tls_close_notify(client) == AI_TLS_ERR_BUSY);
                }
                sent += count;
            }
            assert(sent == sizeof(request) - 1);
            for (calls = 0; received < sizeof(response) - 1 && calls < 10000; ++calls) {
                result = ai_tls_read(client, response + received, sizeof(response) - 1 - received, &count);
                received += count;
                if (result == AI_TLS_CLOSED) {
                    break;
                }
                assert(result == AI_TLS_OK || result == AI_TLS_WANT_READ || result == AI_TLS_WANT_WRITE);
            }
            assert(result == AI_TLS_CLOSED);
            assert(strstr((const char *) response, "tls-loopback-ok") != NULL);
            assert(ai_tls_handshake(client) == AI_TLS_ERR_NOT_READY);
            assert(ai_tls_write(client, request, sizeof(request) - 1, &count) == AI_TLS_ERR_NOT_READY);
            for (calls = 0; calls < 10000; ++calls) {
                result = ai_tls_close_notify(client);
                assert(ai_tls_handshake(client) == AI_TLS_ERR_NOT_READY);
                assert(ai_tls_write(client, request, sizeof(request) - 1, &count) == AI_TLS_ERR_NOT_READY);
                if (result != AI_TLS_WANT_READ && result != AI_TLS_WANT_WRITE) {
                    break;
                }
            }
            assert(result == AI_TLS_OK);
            assert(ai_tls_handshake(client) == AI_TLS_ERR_NOT_READY);
            assert(ai_tls_write(client, request, sizeof(request) - 1, &count) == AI_TLS_ERR_NOT_READY);
        }
        ai_tls_destroy(client);
        closesocket(context.socket);
        WSACleanup();
    }
    free(ca);
    assert(ai_tls_platform_shutdown() == AI_TLS_OK);
    puts("ai_tls_host_test: PASS");
    return 0;
}
