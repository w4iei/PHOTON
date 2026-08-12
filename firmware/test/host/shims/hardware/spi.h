// Host-test shim for pico-sdk hardware/spi.h — just enough for
// board_config.h to compile off-target.
#pragma once
typedef struct spi_inst spi_inst_t;
#define spi0 ((spi_inst_t *)0)
#define spi1 ((spi_inst_t *)1)
