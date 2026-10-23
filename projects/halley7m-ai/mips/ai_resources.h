#ifndef AI_RESOURCES_H
#define AI_RESOURCES_H
#include "ai_assets.h"
#include "ai_config.h"
typedef struct {
    ai_config config;
    ai_asset font, dictionary;
    int mount_result, config_result, font_result, dictionary_result;
    unsigned config_error_line;
} ai_resources;
void ai_resources_load(ai_resources *resources);
#endif
