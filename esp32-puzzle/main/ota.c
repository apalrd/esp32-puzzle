/* OTA server using basic TCP sockets
 * To use, upload via netcat:
 *  nc <device_ip> 6664 < build/firmware.bin (replace with your project name)
 * You can use mdns also, such as <hostname>.local
 */
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_ota_ops.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>


#define OTA_PORT 6664

static const char *TAG = "ota_server";

static void do_ota(const int sock)
{
    int len;
    char rx_buffer[2048];
    esp_ota_handle_t ota_handle = 0;

    //Print partition info before we start
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Next partition type %d subtype %d (offset 0x%08x)",
             update->type, update->subtype, update->address);
    //Start OTA session
    esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota_handle);

    //Print begin to socket
    const char *start_msg = "OTA Start\r\n";
    send(sock, start_msg, strlen(start_msg), 0);

    //Receive data and feed to OTA
    do {
        len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
            esp_ota_abort(ota_handle);
            return;
        } else if (len == 0) {
            //end boot loading
            ESP_LOGW(TAG, "Connection closed");
            esp_ota_end(ota_handle);
            //Set boot partition to the newly received one
            esp_err_t err = esp_ota_set_boot_partition(update);
            //Print error to socket and log
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
                const char *err_msg = "OTA Failed to set boot partition\r\n";
                send(sock, err_msg, strlen(err_msg), 0);
                return;
            }
            //Success message
            ESP_LOGI(TAG, "OTA complete, rebooting to new partition...");
            const char *ok_msg = "OTA complete, rebooting...\r\n";
            send(sock, ok_msg, strlen(ok_msg), 0);
            //Close socket since we won't end this task normally
            shutdown(sock, 0);
            close(sock);
            //Slight delay for logs to flush
            vTaskDelay(20 / portTICK_PERIOD_MS);
            esp_restart();
            //will not return, but good practice
            return;
        } else {
            ESP_LOGI(TAG, "Received %d bytes", len);
            esp_ota_write(ota_handle, rx_buffer, len);
        }
    } while (len > 0);
}

static void ota_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = AF_INET6;
    int ip_protocol = 0;
    struct sockaddr_storage dest_addr;

    struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
    bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
    dest_addr_ip6->sin6_family = AF_INET6;
    dest_addr_ip6->sin6_port = htons(OTA_PORT);
    ip_protocol = IPPROTO_IPV6;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", OTA_PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGI(TAG, "Socket accepted from ip address: %s", addr_str);

        do_ota(sock);
        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);
    xTaskCreate(ota_server_task, "ota_server", 4096, NULL, 5, NULL);

    //If we get to this point, cancel rollback
    esp_ota_mark_app_valid_cancel_rollback();
}