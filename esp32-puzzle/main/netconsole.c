//Network-based console
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

//for logging
static const char *TAG = "netconsole";

//Group and port numbers
#define SRC_PORT 6665
#define DST_PORT 6666
#define NETCONSOLE_GROUP "ff02::666"

static int s_sock = -1;
static vprintf_like_t s_original_vprintf = NULL;
static struct addrinfo *s_res;

static int netconsole_vprintf(const char* format, va_list ap)
{
    if (s_sock < 0) { 
        // Fallback to original vprintf if socket is not available
        return s_original_vprintf(format, ap);
    }
    //Format the log message
    static char sendbuf[1280-40-8]; //IPv6 min MTU minus IPv6 and UDP headers
    int len = vsnprintf(sendbuf, sizeof(sendbuf), format, ap);
    //return on error
    if (len < 0) {
        return len;
    }
    //limit to size of buffer
    if (len > sizeof(sendbuf)) {
        len = sizeof(sendbuf);
    }
    //s_res has already been resolved in init
    int err = sendto(s_sock, sendbuf, len, 0, s_res->ai_addr, s_res->ai_addrlen);
    if (err < 0) {
        return -1;
    }
    return len;
}

//create socket for netconsole
static void netconsole_create_socket(void)
{
    struct sockaddr_in6 saddr = { 0 };
    int  netif_index;
    struct in6_addr if_inaddr = { 0 };
    struct ip6_addr if_ipaddr = { 0 };
    int err = 0;

    s_sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket. Error %d", errno);
        return;
    }

    // Bind the socket to any address
    saddr.sin6_family = AF_INET6;
    saddr.sin6_port = htons(SRC_PORT);
    bzero(&saddr.sin6_addr.un, sizeof(saddr.sin6_addr.un));
    err = bind(s_sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in6));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind socket. Error %d", errno);
        goto err;
    }

    // Select the interface to use as multicast source for this socket.
    // Read interface adapter link-local address and use it
    // to bind the multicast IF to this socket.
    //
    // (Note the interface may have other non-LL IPV6 addresses as well,
    // but it doesn't matter in this context as the address is only
    // used to identify the interface.)
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
    err = setsockopt(s_sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &netif_index,sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_MULTICAST_IF. Error %d", errno);
        goto err;
    }

    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = 1;
    setsockopt(s_sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_MULTICAST_HOPS. Error %d", errno);
        goto err;
    }

    int only = 1; /* IPV6-only socket */
    err = setsockopt(s_sock, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(int));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_V6ONLY. Error %d", errno);
        goto err;
    }
    ESP_LOGI(TAG, "Socket set IPV6-only");

    // All set, socket is configured for sending and receiving
    return;

err:
    close(s_sock);
}

void netconsole_init(void)
{
    netconsole_create_socket();
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create IPv6 multicast socket");
    }
    //run getaddrinfo to resolve multicast address
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_socktype = SOCK_DGRAM,
    };
    hints.ai_family = AF_INET6;
    hints.ai_protocol = 0;
    int err = getaddrinfo(NETCONSOLE_GROUP,
                        NULL,
                        &hints,
                        &s_res);
    if (err < 0) {
        ESP_LOGE(TAG, "getaddrinfo() failed for IPV6 destination address. error: %d", err);
        return;
    }
    //Set port in resolved address
    struct sockaddr_in6 *s6addr = (struct sockaddr_in6 *)s_res->ai_addr;
    s6addr->sin6_port = htons(DST_PORT);

    ESP_LOGI(TAG, "Netconsole initialized, sending to %s port %d", NETCONSOLE_GROUP, DST_PORT);

    // Override the vprintf function to send logs over the network
    s_original_vprintf = esp_log_set_vprintf(&netconsole_vprintf);
}