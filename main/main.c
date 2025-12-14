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
    uint32_t seq_number;
    uint8_t active;
    uint8_t btn[4];
    uint8_t btn_read[4];
    uint8_t phase;
    uint8_t leader;
};

static struct game_data_t my_data, their_data;
static int their_ts;
static struct frame_t mcast_frame;

void game_logic() 
{
    /* Check for updates to their_data */
    frame_recv(&mcast_frame, (char *)&their_data, sizeof(their_data), &their_ts);

    /* Check if we have a new frame from them */
    if (their_data.seq_number > my_data.seq_number) 
    {
        ESP_LOGI(TAG, "Received frame %d phase %d active %d leader %d ts %d",
                 their_data.seq_number,
                 their_data.phase,
                 their_data.active,
                 their_data.leader,
                 their_ts);
        /* Copy data to our struct */
        my_data.phase = their_data.phase;
        my_data.seq_number = their_data.seq_number;

    }

    /* Check if we are active (read RFID) */
    my_data.active = 1; //for testing
    /* Set the leader based on which RFID tag is passed */

    /* Read all four buttons and debounce*/
    static int btn_last[4] = {0};
    for (int i = 0; i < 4; i++) {
        my_data.btn_read[i] = 0; //TODO: read actual button state
        my_data.btn[i] = (my_data.btn_read[i] && !btn_last[i]) ? 1 : 0;
        btn_last[i] = my_data.btn_read[i];
    }

    int changed = 0;
    /* Reset leader state if we are inactive */
    if(my_data.phase < PHASE_IDLE) {
        my_data.leader = 0;
    }
    /* If we are not active and in a phase where we should be active, change our state */
    else if (!my_data.active && my_data.phase >= PHASE_IDLE) {
        my_data.phase = PHASE_INACTIVE;
        changed = 1;
    }
    /* If we are active and they are active, and we are not idle, go to idle */
    else if (my_data.active && their_data.active && my_data.phase < PHASE_IDLE) {
        my_data.phase = PHASE_IDLE;
        changed = 1;
        
    }
    /* If they are a leader and we are a leader, go back to error state */
    else if (my_data.leader && their_data.leader && my_data.phase >= PHASE_IDLE) {
        my_data.phase = PHASE_INCORRECT;
        changed = 1;
    }
    /* If we are in idle, and are the leader, and btn3 is pressed, go to leader1 */
    else if(my_data.phase == PHASE_IDLE && my_data.leader && my_data.btn[2]) {
        my_data.phase = PHASE_LEADER1;
        changed = 1;
    }
    /* If we are in leader1 and not the leader and btn2 is pressed, go to follow1 */
    else if(my_data.phase == PHASE_LEADER1 && !my_data.leader && my_data.btn[1]) {
        my_data.phase = PHASE_FOLLOW1;
        changed = 1;
    }
    /* If we are in follow1 and are the leader and btn4 is pressed, go to leader2 */
    else if(my_data.phase == PHASE_FOLLOW1 && my_data.leader && my_data.btn[3]) {
        my_data.phase = PHASE_LEADER2;
        changed = 1;
    }
    /* If we are in leader2 and not the leader and btn1 is pressed, go to follow2 */
    else if(my_data.phase == PHASE_LEADER2 && !my_data.leader && my_data.btn[0]) {
        my_data.phase = PHASE_FOLLOW2;
        changed = 1;
    }
    /* If we are in follow2 and the leader and btn2 is pressed, go to leader3 */
    else if(my_data.phase == PHASE_FOLLOW2 && my_data.leader && my_data.btn[1]) {
        my_data.phase = PHASE_LEADER3;
        changed = 1;
    }
    /* If we are in leader3 and not the leader and btn3 is pressed, go to follow3 */
    else if(my_data.phase == PHASE_LEADER3 && !my_data.leader && my_data.btn[2]) {
        my_data.phase = PHASE_FOLLOW3;
        changed = 1;
    }
    /* If we are in follow3 and the leader and btn1 is pressed, go to leader4 */
    else if(my_data.phase == PHASE_FOLLOW3 && my_data.leader && my_data.btn[0]) {
        my_data.phase = PHASE_LEADER4;
        changed = 1;
    }
    /* If we are in leader4 and not the leader and btn4 is pressed, go to follow4 */
    else if(my_data.phase == PHASE_LEADER4 && !my_data.leader && my_data.btn[3]) {
        my_data.phase = PHASE_FOLLOW4_COMPLETED;
        changed = 1;
    }
    /* If any button is pressed and we are in idle or above, go to incorrect */
    if(my_data.phase >= PHASE_IDLE) {
        for (int i = 0; i < 4; i++) {
            if (my_data.btn[i]) {
                my_data.phase = PHASE_INCORRECT;
                changed = 1;
                break;
            }
        }
    }

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
    frame_init(&mcast_frame, MULTICAST_GROUP, UDP_PORT);
    while (1) {
        game_logic();
        vTaskDelay(FRAME_RATE_MS / portTICK_PERIOD_MS);
    }
}
