/* Read-only interface to the validated HALLEY7-M SD-SPI transport. */
#ifndef PLAYER_SDSPI_READONLY_H
#define PLAYER_SDSPI_READONLY_H
#include <stddef.h>
#include <stdint.h>
int sdspi_initialize(void);
int sdspi_is_initialized(void);
int sdspi_read(uint8_t *buffer, uint32_t sector, uint32_t count);
uint32_t sdspi_sector_count(void);
uint8_t sdspi_crc7(const uint8_t *data, size_t size);
uint16_t sdspi_crc16(const uint8_t *data, size_t size);
int sdspi_decode_csd(const uint8_t csd[16], uint32_t *sectors);
#endif
