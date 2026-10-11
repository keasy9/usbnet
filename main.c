#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <lwip/apps/mdns.h>
#include <lwip/ip.h>
#include <lwip/udp.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "usb_network.h"

#define RETICULUM_PORT 14242
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1

// usb network addresses
static const ip4_addr_t ownip = IPADDR4_INIT_BYTES(192, 168, 7, 1);
static const ip4_addr_t netmask = IPADDR4_INIT_BYTES(255, 255, 255, 0);
static const ip4_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);

static struct udp_pcb *rnode_pcb = NULL;

// Пересылка данных из Сети (UDP) -> в Радио (UART на Heltec)
static void udp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (p != NULL) {
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            uint8_t *data = (uint8_t *)q->payload;
            for (int i = 0; i < q->len; i++) {
                uart_putc(UART_ID, data[i]);
            }
        }
        pbuf_free(p);
    }
}

int main() {
    // Включаем UART0 для общения с Heltec V3
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // setup USB network
    if (!usb_network_init(&ownip, &netmask, &gateway, true)) {
        return -1;
    }

    // setup DHCP server
    dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, (ip_addr_t *)&ownip, (ip_addr_t *)&netmask, false);

    // Создаем UDP сокет для Reticulum
    rnode_pcb = udp_new();
    if (rnode_pcb != NULL) {
        err_t err = udp_bind(rnode_pcb, IP_ADDR_ANY, RETICULUM_PORT);
        if (err == ERR_OK) {
            udp_recv(rnode_pcb, udp_recv_callback, NULL);
        }
    }

    // enter main loop
    while (true) {
        usb_network_update();

        // Пересылка данных из Радио (UART от Heltec) -> в Сеть (UDP Бродкаст в ПК/Телефон)
        if (uart_is_readable(UART_ID)) {
            uint8_t uart_buf[512];
            int len = 0;
            
            // Быстро вычитываем всё, что насыпалось из UART
            while (uart_is_readable(UART_ID) && len < sizeof(uart_buf)) {
                uart_buf[len++] = uart_getc(UART_ID);
                sleep_us(50); // Небольшая пауза для сборки пакета в буфер
            }

            if (len > 0) {
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
                if (p != NULL) {
                    memcpy(p->payload, uart_buf, len);
                    // Шлем бродкастом на адрес 255.255.255.255, чтобы Sideband на ПК поймал пакет
                    udp_sendto(rnode_pcb, p, IP_ADDR_BROADCAST, RETICULUM_PORT);
                    pbuf_free(p);
                }
            }
        }
    }

    return 0;
}
