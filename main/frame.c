/* Simple code to essentialy emulate a 'CAN-bus' like multicast framing
 * over IPv6 link-local addresses and UDP
 */
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "main.h"

//for logging
static const char *TAG = "frame";

//Group and port numbers
//#define UDP_PORT 6969
//#define MULTICAST_GROUP "ff02::6969"
//#define MULTICAST_TTL 1

//create socket

int frame_init(struct frame_t *frame, char *mcast_group, int port)
{
    struct sockaddr_in6 saddr = { 0 };
    int  netif_index;
    struct in6_addr if_inaddr = { 0 };
    struct ip6_addr if_ipaddr = { 0 };
    struct ipv6_mreq v6imreq = { 0 };
    frame->sock = -1;
    frame->dest = NULL;
    int err = 0;

    ESP_LOGI(TAG, "Initializing frame for group %s port %d", mcast_group, port);

    frame->sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
    if (frame->sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    // Bind the socket to any address
    saddr.sin6_family = AF_INET6;
    saddr.sin6_port = htons(port);
    bzero(&saddr.sin6_addr.un, sizeof(saddr.sin6_addr.un));
    err = bind(frame->sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in6));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }

    // Select the interface to use as multicast source for this socket.
    // Read interface adapter link-local address and use it
    // to bind the multicast IF to this socket.
    err = esp_netif_get_ip6_linklocal(get_example_netif_from_desc("eth_t1s"), (esp_ip6_addr_t*)&if_ipaddr);
    inet6_addr_from_ip6addr(&if_inaddr, &if_ipaddr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IPV6 LL address. Error 0x%x", err);
        goto err;
    }

    // search for netif index
    netif_index = esp_netif_get_netif_impl_index(get_example_netif_from_desc("eth_t1s"));
    if(netif_index < 0) {
        ESP_LOGE(TAG, "Failed to get netif index");
        goto err;
    }
    // Assign the multicast source interface, via its IP
    err = setsockopt(frame->sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &netif_index,sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_MULTICAST_IF. Error %d", errno);
        goto err;
    }

    // Assign multicast TTL to 1 (link-local scope)
    uint8_t ttl = 1;
    setsockopt(frame->sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_MULTICAST_HOPS. Error %d", errno);
        goto err;
    }

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    // Configure multicast address to listen to
    err = inet6_aton(mcast_group, &v6imreq.ipv6mr_multiaddr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV6 multicast address '%s' is invalid.", mcast_group);
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV6 Multicast address %s", inet6_ntoa(v6imreq.ipv6mr_multiaddr));
    ip6_addr_t multi_addr;
    inet6_addr_to_ip6addr(&multi_addr, &v6imreq.ipv6mr_multiaddr);
    if (!ip6_addr_ismulticast(&multi_addr)) {
        ESP_LOGW(TAG, "Configured IPV6 multicast address '%s' is not a valid multicast address. This will probably not work.", mcast_group);
    }
    // Configure source interface
    v6imreq.ipv6mr_interface = (unsigned int)netif_index;
    err = setsockopt(frame->sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                     &v6imreq, sizeof(struct ipv6_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }


    int only = 1; /* IPV6-only socket */
    err = setsockopt(frame->sock, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(int));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_V6ONLY. Error %d", errno);
        goto err;
    }
    ESP_LOGI(TAG, "Socket set IPV6-only");

    //Pre-calculate destination addrinfo struct
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_socktype = SOCK_DGRAM,
    };
    hints.ai_family = AF_INET6;
    hints.ai_protocol = 0;
    err = getaddrinfo(mcast_group,
                        NULL,
                        &hints,
                        &frame->dest);
    if (err != 0) {
        ESP_LOGE(TAG, "getaddrinfo() failed for IPV6 destination address. error: %d", err);
        goto err;
    }

    struct sockaddr_in6 *s6addr = (struct sockaddr_in6 *)frame->dest->ai_addr;
    s6addr->sin6_port = htons(port);

    // All set, socket is configured for sending and receiving
    return 0;

err:
    close(frame->sock);
    return -1;
}


int frame_recv(struct frame_t *frame, char *buf, size_t len, int *time)
{
    /* select() with timeout of zero until no packets arrive 
     * This ensures we have the lastest copy of the data
     */
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = 0,
    };
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(frame->sock, &rfds);
    int s = 1;
    while(s > 0) {
        s = select(frame->sock + 1, &rfds, NULL, NULL, &tv);
        if (s < 0) {
            ESP_LOGE(TAG, "Select failed: errno %d", errno);
            return -1;
        }
        else if (s > 0) {
            if (FD_ISSET(frame->sock, &rfds)) {
                // Incoming datagram received
                struct sockaddr_storage raddr;
                socklen_t socklen = sizeof(raddr);
                int recvd = recvfrom(frame->sock, buf, len, 0,
                                    (struct sockaddr *)&raddr, &socklen);
                if (recvd < 0) {
                    ESP_LOGE(TAG, "multicast recvfrom failed: errno %d", errno);
                    return -1;
                }
                if (time) {
                    // record time of reception
                    *time = esp_log_timestamp();
                }
            }
        }
    }
    return 0;
}

int frame_send(struct frame_t *frame, const char *data, size_t len)
{
    int err = sendto(frame->sock, data, len, 0, frame->dest->ai_addr, frame->dest->ai_addrlen);
    if (err < 0) {
        ESP_LOGE(TAG, "sendto failed. errno: %d", errno);
        return -1;
    }
    return 0;
}