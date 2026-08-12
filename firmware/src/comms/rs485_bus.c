// Plan-1 transport: shared half-duplex RS-485, bridge-mastered.
//
// TX: per-frame DMA into the UART FIFO; DE is raised 25 us before the first
//     bit and dropped 25 us after the shifter drains — sequenced by a polled
//     state machine, never by busy-waiting the whole frame (v1 blocked the
//     CPU for up to 1.3 ms per frame).
// RX: a free-running DMA channel copies UART DR into a power-of-two ring;
//     transport_task() drains new bytes into a linear assembly buffer and
//     runs the O(n) frame parser over it.
#include "comms/transport.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/time.h"

#include "board_config.h"

#define RX_RING_BITS 11
#define RX_RING_SIZE (1u << RX_RING_BITS)  // 2048
#define ASSEMBLY_SIZE 512
#define TX_QUEUE_SLOTS 8

typedef struct {
    uint8_t data[PHOTON_FRAME_MAX_LEN];
    uint16_t len;
} tx_slot_t;

typedef struct {
    tx_slot_t slots[TX_QUEUE_SLOTS];
    uint32_t head, tail;
} tx_queue_t;

typedef enum {
    TX_IDLE,
    TX_PRE_GUARD,
    TX_SENDING,
    TX_DRAIN,
    TX_POST_GUARD,
} tx_state_t;

static struct {
    uart_inst_t *uart;
    uint8_t de_pin;
    uint8_t own_addr;
    transport_rx_cb_t on_rx;

    int tx_dma, rx_dma;
    uint8_t rx_ring[RX_RING_SIZE] __attribute__((aligned(RX_RING_SIZE)));
    uint32_t rx_roff;  // ring offset consumed so far (0..RX_RING_SIZE-1)

    uint8_t assembly[ASSEMBLY_SIZE];
    size_t assembly_len;

    tx_queue_t prio_q, bulk_q;
    tx_queue_t *tx_active_q;  // queue whose head frame is on the wire
    tx_state_t tx_state;
    absolute_time_t tx_deadline;

    transport_stats_t stats;
    frame_parse_stats_t parse_stats;
} bus;

// ---------------------------------------------------------------------------

static bool queue_push(tx_queue_t *q, const photon_frame_t *f) {
    if (q->tail - q->head >= TX_QUEUE_SLOTS) {
        return false;
    }
    tx_slot_t *s = &q->slots[q->tail % TX_QUEUE_SLOTS];
    s->len = (uint16_t)frame_encode(f, s->data);
    if (s->len == 0) {
        return false;  // over-length payload rejected by the encoder
    }
    q->tail++;
    return true;
}

static tx_slot_t *queue_peek(tx_queue_t *q) {
    return q->head == q->tail ? NULL : &q->slots[q->head % TX_QUEUE_SLOTS];
}

void transport_init(bool use_host_pinout, bool terminate, uint8_t own_addr,
                    transport_rx_cb_t on_rx) {
    memset(&bus.stats, 0, sizeof bus.stats);
    memset(&bus.parse_stats, 0, sizeof bus.parse_stats);
    bus.own_addr = own_addr;
    bus.on_rx = on_rx;
    bus.assembly_len = 0;
    bus.rx_roff = 0;
    bus.tx_active_q = NULL;
    bus.tx_state = TX_IDLE;
    bus.prio_q.head = bus.prio_q.tail = 0;
    bus.bulk_q.head = bus.bulk_q.tail = 0;

    uint tx_pin, rx_pin, term_pin;
    if (use_host_pinout) {
        bus.uart = PHOTON_HOST_UART;
        tx_pin = PHOTON_HOST_TX;
        rx_pin = PHOTON_HOST_RX;
        bus.de_pin = PHOTON_HOST_DE;
        term_pin = PHOTON_HOST_TERM;
    } else {
        bus.uart = PHOTON_RS485_UART;
        tx_pin = PHOTON_RS485_TX;
        rx_pin = PHOTON_RS485_RX;
        bus.de_pin = PHOTON_RS485_DE;
        term_pin = PHOTON_RS485_TERM;
    }

    uart_init(bus.uart, PHOTON_RS485_BAUD);
    uart_set_format(bus.uart, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(bus.uart, true);
    // RP2350 pin mux: UART TX/RX live on F2 only for pins 4k+0/4k+1; on
    // 4k+2/4k+3 (the sensor board's GPIO22/23) F2 is CTS/RTS and TX/RX are
    // on the F11 UART_AUX function. CircuitPython did this internally,
    // which is why the legacy stack worked on the same pins.
    gpio_set_function(tx_pin, (tx_pin & 2) ? GPIO_FUNC_UART_AUX : GPIO_FUNC_UART);
    gpio_set_function(rx_pin, (rx_pin & 2) ? GPIO_FUNC_UART_AUX : GPIO_FUNC_UART);

    gpio_init(bus.de_pin);
    gpio_disable_pulls(bus.de_pin);
    gpio_put(bus.de_pin, 0);
    gpio_set_dir(bus.de_pin, GPIO_OUT);

    // Bus endpoints terminate; matches the deployed configuration (host
    // terminates, sensor boards use their DIP switches instead).
    gpio_init(term_pin);
    gpio_put(term_pin, terminate ? 1 : 0);
    gpio_set_dir(term_pin, GPIO_OUT);

    // RX ring DMA: UART DR -> ring, paced by the RX DREQ, huge count,
    // re-armed from transport_task long before exhaustion.
    bus.rx_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(bus.rx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RX_RING_BITS);
    channel_config_set_dreq(&c, uart_get_dreq(bus.uart, false));
    dma_channel_configure(bus.rx_dma, &c, bus.rx_ring,
                          &uart_get_hw(bus.uart)->dr, 0x0FFFFFFF, true);

    // TX DMA: assembly -> UART DR, started per frame.
    bus.tx_dma = dma_claim_unused_channel(true);
}

bool transport_send(const photon_frame_t *f, bool prio) {
    photon_frame_t frame = *f;
    frame.src = bus.own_addr;
    return queue_push(prio ? &bus.prio_q : &bus.bulk_q, &frame);
}

// ---------------------------------------------------------------------------
// TX state machine
// ---------------------------------------------------------------------------

static void tx_start(tx_slot_t *slot) {
    dma_channel_config c = dma_channel_get_default_config(bus.tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, uart_get_dreq(bus.uart, true));
    dma_channel_configure(bus.tx_dma, &c, &uart_get_hw(bus.uart)->dr,
                          slot->data, slot->len, true);
}

static void tx_pump(void) {
    switch (bus.tx_state) {
        case TX_IDLE: {
            tx_queue_t *q = queue_peek(&bus.prio_q) ? &bus.prio_q
                          : queue_peek(&bus.bulk_q) ? &bus.bulk_q
                                                    : NULL;
            if (q == NULL) {
                return;
            }
            bus.tx_active_q = q;  // latch: the choice must not change mid-send
            gpio_put(bus.de_pin, 1);
            bus.tx_deadline = make_timeout_time_us(PHOTON_DE_GUARD_US);
            bus.tx_state = TX_PRE_GUARD;
            break;
        }
        case TX_PRE_GUARD:
            if (time_reached(bus.tx_deadline)) {
                tx_start(queue_peek(bus.tx_active_q));
                bus.tx_state = TX_SENDING;
            }
            break;
        case TX_SENDING:
            if (!dma_channel_is_busy(bus.tx_dma)) {
                bus.tx_state = TX_DRAIN;
            }
            break;
        case TX_DRAIN:
            // Wait for the UART FIFO + shifter to empty.
            if (!(uart_get_hw(bus.uart)->fr & UART_UARTFR_BUSY_BITS)) {
                bus.tx_deadline = make_timeout_time_us(PHOTON_DE_GUARD_US);
                bus.tx_state = TX_POST_GUARD;
            }
            break;
        case TX_POST_GUARD:
            if (time_reached(bus.tx_deadline)) {
                gpio_put(bus.de_pin, 0);
                bus.tx_active_q->head++;
                bus.tx_active_q = NULL;
                bus.stats.tx_frames++;
                bus.tx_state = TX_IDLE;
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// RX drain + parse
// ---------------------------------------------------------------------------

static void rx_pump(void) {
    // The DMA write pointer is the live ring offset; deriving it from
    // write_addr (not transfer_count) keeps the math valid across re-arms.
    uint32_t woff = (uint32_t)(dma_channel_hw_addr(bus.rx_dma)->write_addr -
                               (uintptr_t)bus.rx_ring) & (RX_RING_SIZE - 1);

    while (bus.rx_roff != woff) {
        if (bus.assembly_len == ASSEMBLY_SIZE) {
            // Assembly full with no parseable frame: keep the newest half.
            memmove(bus.assembly, bus.assembly + ASSEMBLY_SIZE / 2, ASSEMBLY_SIZE / 2);
            bus.assembly_len = ASSEMBLY_SIZE / 2;
        }
        // Copy in contiguous spans (up to ring wrap / write offset / room).
        uint32_t span = (bus.rx_roff < woff ? woff : RX_RING_SIZE) - bus.rx_roff;
        size_t room = ASSEMBLY_SIZE - bus.assembly_len;
        if (span > room) {
            span = (uint32_t)room;
        }
        memcpy(bus.assembly + bus.assembly_len, bus.rx_ring + bus.rx_roff, span);
        bus.assembly_len += span;
        bus.rx_roff = (bus.rx_roff + span) & (RX_RING_SIZE - 1);
    }

    for (;;) {
        size_t consumed = 0;
        photon_frame_t f;
        frame_parse_result_t r = frame_parse(bus.assembly, bus.assembly_len,
                                             &consumed, &f, &bus.parse_stats);
        if (consumed > 0) {
            memmove(bus.assembly, bus.assembly + consumed,
                    bus.assembly_len - consumed);
            bus.assembly_len -= consumed;
        }
        if (r != FRAME_PARSE_OK) {
            break;
        }
        bus.stats.rx_frames++;
        // Deliver every valid frame; the protocol layer does the address
        // filtering. RS-485 is a shared medium, and a USB-connected node
        // deliberately snoops other nodes' event batches to mirror the
        // whole instrument over its own MIDI port.
        bus.on_rx(&f);
    }

    // Re-arm the RX DMA long before its huge count runs out (write_addr and
    // the ring wrap are preserved; only the countdown restarts).
    if (dma_channel_hw_addr(bus.rx_dma)->transfer_count < RX_RING_SIZE) {
        dma_channel_set_trans_count(bus.rx_dma, 0x0FFFFFFF, true);
    }
}

void transport_task(void) {
    tx_pump();
    rx_pump();
}

const transport_stats_t *transport_stats(void) {
    return &bus.stats;
}

const frame_parse_stats_t *transport_parse_stats(void) {
    return &bus.parse_stats;
}
