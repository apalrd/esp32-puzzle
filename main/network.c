/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "ethernet_init.h"
#include "esp_mac.h"
#include "mdns.h"

#define MAX_IP6_ADDRS_PER_NETIF (5)

static const char *TAG = "network";

static SemaphoreHandle_t s_semph_get_ip6_addrs = NULL;


/* types of ipv6 addresses to be displayed on ipv6 events */
static const char *ipv6_addr_types_to_str[6] = {
    "ESP_IP6_ADDR_IS_UNKNOWN",
    "ESP_IP6_ADDR_IS_GLOBAL",
    "ESP_IP6_ADDR_IS_LINK_LOCAL",
    "ESP_IP6_ADDR_IS_SITE_LOCAL",
    "ESP_IP6_ADDR_IS_UNIQUE_LOCAL",
    "ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6"
};

/** Event handler for Ethernet events */
static void eth_on_got_ipv6(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    esp_ip6_addr_type_t ipv6_type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
    ESP_LOGI(TAG, "Got IPv6 event: Interface \"%s\" address: " IPV6STR ", type: %s", esp_netif_get_desc(event->esp_netif),
             IPV62STR(event->ip6_info.ip), ipv6_addr_types_to_str[ipv6_type]);
    //Require a LLA to consider the interface ready
    if (ipv6_type == ESP_IP6_ADDR_IS_LINK_LOCAL) {
        xSemaphoreGive(s_semph_get_ip6_addrs);
    }
}

static void on_eth_event(void *esp_netif, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_ERROR_CHECK(esp_netif_create_ip6_linklocal(esp_netif));
        break;
    default:
        break;
    }
}

static esp_eth_handle_t *s_eth_handles = NULL;
static uint8_t s_eth_count = 0;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static esp_netif_t *s_eth_netif = NULL;
static char s_hostname[32] = {0};

static esp_netif_t *eth_start(void)
{
    ESP_ERROR_CHECK(ethernet_init_all(&s_eth_handles, &s_eth_count));

    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    // Warning: the interface desc is used in tests to capture actual connection details (IP, gw, mask)
    esp_netif_config.if_desc = "eth_t1s";
    esp_netif_config.route_prio = 64;
    esp_netif_config_t netif_config = {
        .base = &esp_netif_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
    };
    s_eth_netif = esp_netif_new(&netif_config);

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handles[0]);
    esp_netif_attach(s_eth_netif, s_eth_glue);

    //Get MAC, generate hostname from last 3 bytes of MAC
    uint8_t mac[6];
    esp_netif_get_mac(s_eth_netif, mac);
    snprintf(s_hostname, sizeof(s_hostname), "mant1s_%02x%02x%02x", mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(s_eth_netif, s_hostname);
    ESP_LOGI(TAG, "Hostname is %s", s_hostname);

    // Register user defined event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &on_eth_event, s_eth_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &eth_on_got_ipv6, NULL));

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handles[0]));

    return s_eth_netif;
}

static void eth_stop(void)
{
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6, &eth_on_got_ipv6));
    ESP_ERROR_CHECK(esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_CONNECTED, &on_eth_event));
    ESP_ERROR_CHECK(esp_eth_stop(s_eth_handles[0]));
    ESP_ERROR_CHECK(esp_eth_del_netif_glue(s_eth_glue));
    esp_netif_destroy(s_eth_netif);
    ethernet_deinit_all(s_eth_handles);

    s_eth_glue = NULL;
    s_eth_netif = NULL;
    s_eth_handles = NULL;
    s_eth_count = 0;
}


/* tear down connection, release resources */
static void eth_shutdown(void)
{
    if (s_semph_get_ip6_addrs == NULL) {
        return;
    }
    vSemaphoreDelete(s_semph_get_ip6_addrs);
    s_semph_get_ip6_addrs = NULL;
    eth_stop();
}

//print all IPs of netifs
static esp_err_t print_all_ips_tcpip(void* ctx)
{
    // iterate over active interfaces
    esp_netif_t *netif = NULL;
    while ((netif = esp_netif_next_unsafe(netif)) != NULL) {
        ESP_LOGI(TAG, "Connected to %s", esp_netif_get_desc(netif));
        esp_ip6_addr_t ip6[MAX_IP6_ADDRS_PER_NETIF];
        int ip6_addrs = esp_netif_get_all_ip6(netif, ip6);
        for (int j = 0; j < ip6_addrs; ++j) {
            esp_ip6_addr_type_t ipv6_type = esp_netif_ip6_get_addr_type(&(ip6[j]));
            ESP_LOGI(TAG, "- IPv6 address: " IPV6STR ", type: %s", IPV62STR(ip6[j]), ipv6_addr_types_to_str[ipv6_type]);
        }
    }
    return ESP_OK;
}

esp_err_t eth_connect(void)
{
    s_semph_get_ip6_addrs = xSemaphoreCreateBinary();
    if (s_semph_get_ip6_addrs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    eth_start();
    ESP_LOGI(TAG, "Waiting for IP(s).");
    xSemaphoreTake(s_semph_get_ip6_addrs, portMAX_DELAY);

    ESP_ERROR_CHECK(esp_register_shutdown_handler(&eth_shutdown));

    // Print all IPs in TCPIP context to avoid potential races of removing/adding netifs when iterating over the list
    esp_netif_tcpip_exec(print_all_ips_tcpip, NULL);

    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        printf("MDNS Init failed: %d\n", err);
        return err;
    }

    //set hostname
    mdns_hostname_set(s_hostname);
    //add services
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_ota", "_tcp", 6664, NULL, 0);

    return ESP_OK;
}


//Separate function to print hostname, once netconsole is started
void eth_print_hostname() {
    ESP_LOGI(TAG, "Hostname: %s", s_hostname);
}