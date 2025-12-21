/* Puzzle main code */
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
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "main.h"

//for logging
static const char *TAG = "main";

//Time period between sending data packets
#define FRAME_RATE_MS 20

//Group and port numbers
#define UDP_PORT 6969
#define MULTICAST_GROUP "ff02::6969"

/* game input and output pins */
#define PIN_BTN_COUNT 4
//static const int pin_btn[PIN_BTN_COUNT] = { 36, 37, 38, 39 };
#define PIN_LED_COUNT 6
//static const int pin_led[PIN_LED_COUNT] = {14, 12, 13, 15, 2, 4};

/* game_state for puzzling */
enum game_phase_t {
    PHASE_INACTIVE, //At least one station is not active
    PHASE_IDLE,     //Both stations active, waiting for input
    PHASE_LEADER1,
    PHASE_FOLLOW1,
    PHASE_LEADER2,
    PHASE_FOLLOW2,
    PHASE_LEADER3,
    PHASE_FOLLOW3,
    PHASE_LEADER4,
    PHASE_FOLLOW4_COMPLETED,
    PHASE_INCORRECT, //Incorrect input entered, flash red
};

/* Structure we will pass around the network */
struct game_data_t {
    uint32_t seq_number;    //Increments whenever state is modified
    uint8_t active;         //Tag received
    uint8_t btn;            //Current button press (= 0 no button, 255 = multiples)
    uint8_t phase;          //Current phase
    uint8_t leader;
};

/* Global state data modified by funcs */
static struct game_data_t my_data, their_data;
static int their_ts;
static struct frame_t mcast_frame;


/* Config GPIO inputs */
static void io_config()
{
    //
#if 0
gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << BUTTON_PIN),   // Select GPIO 4
    .mode = GPIO_MODE_INPUT,                  // Set as input
    .pull_up_en = GPIO_PULLUP_ENABLE,     // Enable internal pull-up
    .pull_down_en = GPIO_PULLDOWN_DISABLE, // Disable pull-down
    .intr_type = GPIO_INTR_DISABLE        // Disable interrupts
};
gpio_config(&io_conf);
#endif
}

/* Read GPIO inputs / debounce */
static void io_input()
{
    /* Input GPIOs */
    my_data.btn = 0;
    
}

/* Process LED outputs */
static void io_output()
{

}

void game_logic() 
{
    /* Check for updates to their_data */
    frame_recv(&mcast_frame, (char *)&their_data, sizeof(their_data), &their_ts);

    /* Check if we have a new frame from them */
    if (their_data.seq_number > my_data.seq_number) 
    {
        ESP_LOGI(TAG, "Received frame %d phase %d active %d ts %d",
                 their_data.seq_number,
                 their_data.phase,
                 their_data.active,
                 their_ts);
        /* Copy data to our struct */
        my_data.phase = their_data.phase;
        my_data.seq_number = their_data.seq_number;

    }

    /* Check if we are active (read RFID) */
    my_data.active = 1; //for testing
    /* Set the leader based on which RFID tag is passed */



    int changed = 0;
 

    /* If we changed any data, increment sequence number */
    if (changed) {
        my_data.seq_number++;
        ESP_LOGI(TAG, "State changed to phase %d active %d leader %d",
                 my_data.phase,
                 my_data.active,
                 my_data.leader);
    }

    /* Send our own state */
    frame_send(&mcast_frame, (const char *)&my_data, sizeof(my_data));
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

    /* Start Ethernet */
    ESP_ERROR_CHECK(eth_connect());
    /* Start Netconsole */
    netconsole_init();
    /* Start OTA */
    ota_init();
    /* Print hostname (to netconsole) */
    eth_print_hostname();

    /* Run game logic */
    rfid_init();
    frame_init(&mcast_frame, MULTICAST_GROUP, UDP_PORT);
    while (1) {
        game_logic();
        vTaskDelay(FRAME_RATE_MS / portTICK_PERIOD_MS);
    }
}
