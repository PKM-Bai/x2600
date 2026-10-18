#ifndef REGBA_SDSPI_H
#define REGBA_SDSPI_H

#include <stddef.h>
#include <stdint.h>

/* Owned exclusively by the storage task. No card-detect GPIO is wired. */
int sdspi_initialize(void);
int sdspi_is_initialized(void);
int sdspi_read(uint8_t *buffer, uint32_t sector, uint32_t count);
int sdspi_write(const uint8_t *buffer, uint32_t sector, uint32_t count);
int sdspi_sync(void);
uint32_t sdspi_sector_count(void);
/* Filesystem-owner diagnostic only: GPIO CMD0/CMD8, leaves media uninitialized. */
int sdspi_gpio_probe(void);
/* Pure helpers exported for native regression tests. */
uint8_t sdspi_crc7(const uint8_t *data, size_t size);
uint16_t sdspi_crc16(const uint8_t *data, size_t size);
int sdspi_decode_csd(const uint8_t csd[16], uint32_t *sectors);

#endif
