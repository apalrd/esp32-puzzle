#pragma once
#include "lwip/sockets.h"

/* netconsole.c */
void netconsole_init(void);

/* network.c */
esp_err_t eth_connect(void);
void eth_print_hostname(void);

/* ota.c */
void ota_init(void);

/* frame.c */
struct frame_t {
    int sock;
    struct addrinfo *dest;
};
int frame_init(struct frame_t *frame, char *mcast_group, int port);
int frame_recv(struct frame_t *frame, char *buf, size_t len, int *time);
int frame_send(struct frame_t *frame, const char *data, size_t len);

/* rfid.c */
void rfid_init(void);