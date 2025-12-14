/* UDP MultiCast Send/Receive Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
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

//for logging
static const char *TAG = "main";

//Time period between sending data packets
#define FRAME_RATE_MS 20

//Group and port numbers
#define UDP_PORT 6969
#define MULTICAST_GROUP "ff02::6969"
#define MULTICAST_TTL 1

//create socket
static int create_multicast_ipv6_socket(void)
{
    struct sockaddr_in6 saddr = { 0 };
    int  netif_index;
    struct in6_addr if_inaddr = { 0 };
    struct ip6_addr if_ipaddr = { 0 };
    struct ipv6_mreq v6imreq = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }

    // Bind the socket to any address
    saddr.sin6_family = AF_INET6;
    saddr.sin6_port = htons(UDP_PORT);
    bzero(&saddr.sin6_addr.un, sizeof(saddr.sin6_addr.un));
    err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in6));
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
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &netif_index,sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_MULTICAST_IF. Error %d", errno);
        goto err;
    }

    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_MULTICAST_HOPS. Error %d", errno);
        goto err;
    }

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    // Configure multicast address to listen to
    err = inet6_aton(MULTICAST_GROUP, &v6imreq.ipv6mr_multiaddr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV6 multicast address '%s' is invalid.", MULTICAST_GROUP);
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV6 Multicast address %s", inet6_ntoa(v6imreq.ipv6mr_multiaddr));
    ip6_addr_t multi_addr;
    inet6_addr_to_ip6addr(&multi_addr, &v6imreq.ipv6mr_multiaddr);
    if (!ip6_addr_ismulticast(&multi_addr)) {
        ESP_LOGW(TAG, "Configured IPV6 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_GROUP);
    }
    // Configure source interface
    v6imreq.ipv6mr_interface = (unsigned int)netif_index;
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                     &v6imreq, sizeof(struct ipv6_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }


    int only = 1; /* IPV6-only socket */
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(int));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_V6ONLY. Error %d", errno);
        goto err;
    }
    ESP_LOGI(TAG, "Socket set IPV6-only");

    // All set, socket is configured for sending and receiving
    return sock;

err:
    close(sock);
    return -1;
}

static void mcast_example_task(void *pvParameters)
{
    while (1) {
        int sock;

        sock = create_multicast_ipv6_socket();
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create IPv6 multicast socket");
        }

        if (sock < 0) {
            // Sock failed to create, try again after delay
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        struct sockaddr_in6 sdestv6 = {
            .sin6_family = PF_INET6,
            .sin6_port = htons(UDP_PORT),
        };
        // We know this inet_aton will pass because we did it above already
        inet6_aton(MULTICAST_GROUP, &sdestv6.sin6_addr);

        // Loop waiting for UDP received, and sending UDP packets if we don't
        // see any.
        int err = 1;
        while (err > 0) {
            struct timeval tv = {
                .tv_sec = 2,
                .tv_usec = 0,
            };
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);

            int s = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (s < 0) {
                ESP_LOGE(TAG, "Select failed: errno %d", errno);
                err = -1;
                break;
            }
            else if (s > 0) {
                if (FD_ISSET(sock, &rfds)) {
                    // Incoming datagram received
                    char recvbuf[48];
                    char raddr_name[32] = { 0 };

                    struct sockaddr_storage raddr; // Large enough for both IPv4 or IPv6
                    socklen_t socklen = sizeof(raddr);
                    int len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0,
                                       (struct sockaddr *)&raddr, &socklen);
                    if (len < 0) {
                        ESP_LOGE(TAG, "multicast recvfrom failed: errno %d", errno);
                        err = -1;
                        break;
                    }

                    // Get the sender's address as a string
                    if (raddr.ss_family== PF_INET6) {
                        inet6_ntoa_r(((struct sockaddr_in6 *)&raddr)->sin6_addr, raddr_name, sizeof(raddr_name)-1);
                    }
                    ESP_LOGI(TAG, "received %d bytes from %s:", len, raddr_name);

                    recvbuf[len] = 0; // Null-terminate whatever we received and treat like a string...
                    ESP_LOGI(TAG, "%s", recvbuf);
                }
            }
            else { // s == 0
                // Timeout passed with no incoming data, so send something!
                static int send_count;
                const char sendfmt[] = "Multicast #%d sent by ESP32\n";
                char sendbuf[48];
                char addrbuf[32] = { 0 };
                int len = snprintf(sendbuf, sizeof(sendbuf), sendfmt, send_count++);
                if (len > sizeof(sendbuf)) {
                    ESP_LOGE(TAG, "Overflowed multicast sendfmt buffer!!");
                    send_count = 0;
                    err = -1;
                    break;
                }

                struct addrinfo hints = {
                    .ai_flags = AI_PASSIVE,
                    .ai_socktype = SOCK_DGRAM,
                };
                struct addrinfo *res;
                hints.ai_family = AF_INET6;
                hints.ai_protocol = 0;
                err = getaddrinfo(MULTICAST_GROUP,
                                  NULL,
                                  &hints,
                                  &res);
                if (err < 0) {
                    ESP_LOGE(TAG, "getaddrinfo() failed for IPV6 destination address. error: %d", err);
                    break;
                }

                struct sockaddr_in6 *s6addr = (struct sockaddr_in6 *)res->ai_addr;
                s6addr->sin6_port = htons(UDP_PORT);
                inet6_ntoa_r(s6addr->sin6_addr, addrbuf, sizeof(addrbuf)-1);
                ESP_LOGI(TAG, "Sending to IPV6 multicast address %s port %d...",  addrbuf, UDP_PORT);
                err = sendto(sock, sendbuf, len, 0, res->ai_addr, res->ai_addrlen);
                freeaddrinfo(res);
                if (err < 0) {
                    ESP_LOGE(TAG, "IPV6 sendto failed. errno: %d", errno);
                    break;
                }
            }
        }

        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);
    }
}



void app_main(void)
{
    ESP_LOGI(TAG, "App Start");
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    esp_err_t eth_connect(void);
    ESP_ERROR_CHECK(eth_connect());

    void netconsole_init(void);
    netconsole_init();
    void ota_init(void);
    ota_init();

    xTaskCreate(&mcast_example_task, "mcast_task", 8192, NULL, 5, NULL);
}
