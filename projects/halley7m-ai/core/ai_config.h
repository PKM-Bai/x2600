#ifndef AI_CONFIG_H
#define AI_CONFIG_H
#include <stddef.h>
typedef enum { AI_PROVIDER_DEEPSEEK, AI_PROVIDER_BAILIAN } ai_provider;
typedef struct {
    ai_provider provider;
    char model[80], api_key[512], workspace_id[128];
    char wifi_ssid[33], wifi_password[65];
} ai_config;
/* Read-only UTF-8 key=value file. Error messages never include values. */
int ai_config_parse(ai_config *out, const char *data, size_t bytes, unsigned *error_line);
#endif
