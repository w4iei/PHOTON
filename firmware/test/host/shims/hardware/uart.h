// Host-test shim for pico-sdk hardware/uart.h.
#pragma once
typedef struct uart_inst uart_inst_t;
#define uart0 ((uart_inst_t *)0)
#define uart1 ((uart_inst_t *)1)
