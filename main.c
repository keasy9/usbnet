#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <lwip/apps/mdns.h>
#include <lwip/ip.h>
#include <lwip/tcp.h>
#include <pico/stdlib.h>
#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "usb_network.h"

#define TCP_PORT 8232
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1

static const ip4_addr_t ownip = IPADDR4_INIT_BYTES(192, 168, 7, 1);
static const ip4_addr_t netmask = IPADDR4_INIT_BYTES(255, 255, 255, 0);
static const ip4_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);

static struct tcp_pcb *active_client = NULL;

// Пересылка данных из Сети (TCP) -> в UART Heltec
static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (p != NULL) {
        tcp_recved(tpcb, p->tot_len);
        struct pbuf *q;
        for (q = p; q != NULL; q = q->next) {
            uint8_t *data = (uint8_t *)q->payload;
            for (int i = 0; i < q->len; i++) {
                uart_putc(UART_ID, data[i]);
            }
        }
        pbuf_free(p);
    } else {
        active_client = NULL;
        tcp_close(tpcb);
    }
    return ERR_OK;
}

static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err) {
    active_client = newpcb;
    tcp_recv(newpcb, tcp_recv_callback);
    return ERR_OK;
}

int main() {
    // Включаем аппаратный UART для связи с Heltec
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Инициализируем USB-сеть
    if (!usb_network_init(&ownip, &netmask, &gateway, true)) return -1;

    // Поднимаем DHCP сервер (исправлено объявление struct)
    struct dhcp_server dhcp_server;
    dhcp_server_init(&dhcp_server, (ip_addr_t *)&ownip, (ip_addr_t *)&netmask, false);

    // Запускаем TCP сервер для трансляции RNodeInterface
    struct tcp_pcb *server_pcb = tcp_new();
    if (server_pcb) {
        if (tcp_bind(server_pcb, IP_ADDR_ANY, TCP_PORT) == ERR_OK) {
            server_pcb = tcp_listen(server_pcb);
            tcp_accept(server_pcb, tcp_accept_callback);
        }
    }

    // Буфер для вычитывания UART
    uint8_t uart_buf[256];

    while (true) {
        usb_network_update();

        // Пересылка данных из UART Heltec -> в Сеть (TCP на ПК)
        if (active_client && uart_is_readable(UART_ID)) {
            int len = 0;
            // Собираем байты из UART, пока они есть и пока не переполнился буфер
            while (uart_is_readable(UART_ID) && len < sizeof(uart_buf)) {
                uart_buf[len++] = uart_getc(UART_ID);
                sleep_us(10);
            }
            if (len > 0) {
                tcp_write(active_client, uart_buf, len, TCP_WRITE_FLAG_COPY);
                tcp_output(active_client);
            }
        }
    }
    return 0;
}
