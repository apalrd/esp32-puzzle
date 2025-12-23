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
#define FRAME_RATE_MS 50

//Group and port numbers
#define UDP_PORT 6969
#define MULTICAST_GROUP "ff02::6969"

/* game input and output pins */
#define PIN_BTN_COUNT 4
static const int pin_btn[PIN_BTN_COUNT] = { 37, 38, 39, 35 };
#define PIN_LED_COUNT 6
static const int pin_led[PIN_LED_COUNT] = {14, 12, 13, 15, 33, 32};

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
    PHASE_FOLLOWER
};

/* Structure we will pass around the network */
struct game_data_t {
    uint32_t seq_number;    //Increments whenever state is modified
    uint8_t active;         //Tag received
    uint8_t btn;            //Current button press (= 0 no button, 255 = multiples)
    uint8_t phase;          //Current phase
    uint8_t leader;
    uint8_t output;         //Current output pattern (bitmask)
};

/* Global state data modified by funcs */
static struct game_data_t my_data, their_data;
static int their_ts;
static struct frame_t mcast_frame;


/* Config GPIO inputs */
static void io_init()
{
    for(int i = 0; i < PIN_BTN_COUNT;i++)
    {
        ESP_LOGI(TAG,"Configuring IO Input %d (pin %d)",i,pin_btn[i]);
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pin_btn[i]),   // Select GPIO
            .mode = GPIO_MODE_INPUT,                // Set as input
            .pull_up_en = GPIO_PULLUP_ENABLE,       // Enable internal pull-up
            .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Disable pull-down
            .intr_type = GPIO_INTR_DISABLE          // Disable interrupts
        };
        gpio_config(&io_conf);
    }
    for(int i = 0; i < PIN_LED_COUNT;i++)
    {
        ESP_LOGI(TAG,"Configuring IO Output %d (pin %d)",i,pin_led[i]);
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pin_led[i]),   // Select GPIO 4
            .mode = GPIO_MODE_OUTPUT_OD,               // Set as output
            .pull_up_en = GPIO_PULLUP_DISABLE,      // Enable internal pull-up
            .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Disable pull-down
            .intr_type = GPIO_INTR_DISABLE          // Disable interrupts
        };
        gpio_config(&io_conf);
    }
}

/* Read GPIO inputs / debounce */
static void io_input()
{
    /* Input GPIOs */
    static uint8_t btn_last = 0;
    my_data.btn = 0;
    int tmp[PIN_BTN_COUNT];
    for(int i = 0; i < PIN_BTN_COUNT;i++)
    { 
        /* Read this GPIO */
        tmp[i] = !gpio_get_level(pin_btn[i]);
        if(tmp[i] && my_data.btn) my_data.btn = 255; //Multiple buttons
        else if(tmp[i]) my_data.btn = i+1; //Single button
    }
    if(my_data.btn != btn_last) ESP_LOGI(TAG,"Got GPIO %3d [%d %d %d %d]",my_data.btn,tmp[0],tmp[1],tmp[2],tmp[3]);
    btn_last = my_data.btn;
}

/* Process LED outputs */
static uint8_t g_output = 0;
static void io_output()
{
    static uint8_t output_last = 0;
    if(g_output != output_last) ESP_LOGI(TAG,"Writing Outputs %x",g_output);
    output_last = g_output;
    /* Output GPIOs */
    for(int i = 0; i < PIN_LED_COUNT;i++)
    { 
        /* Write this GPIO */
        gpio_set_level(pin_led[i],(g_output & (1 << i)) ? 0 : 1);
    }

}

void game_logic() 
{
    /* Update our own active status */
    my_data.active = g_rfid_tag_active;

    /* Check for updates to their_data */
    frame_recv(&mcast_frame, (char *)&their_data, sizeof(their_data), &their_ts);

    /* Check if we have a new frame from them */
    static uint8_t their_last_phase = 0;
    static uint8_t their_last_btn = 0;
    static uint8_t my_last_btn = 0;
    static uint8_t my_last_phase = 0;
    int their_btn_changed = (their_last_btn != their_data.btn && their_data.btn != 0) ? 1 : 0;
    int my_btn_changed = (my_last_btn != my_data.btn && my_data.btn != 0) ? 1 : 0;
    int changed = 0;

    /* Update our own Active LED */
    g_output = 0;
    my_data.output = 0;
    if(g_rfid_tag_active) g_output |= (1<<5);
    my_data.active = g_rfid_tag_active;

    /* If we are active 2, we are leader */
    if(g_rfid_tag_active == 2)
    {
        /* If they are inactive, clear output */
        if(!their_data.active)
        {
            /* Reset phase */
            changed = 1;
            my_data.phase = PHASE_INACTIVE;
        }
        else 
        {
            /* Logic from each phase */
            switch(my_data.phase)
            {
            case PHASE_INACTIVE:
                //Become active (since they and us are both active)
                my_data.phase = PHASE_IDLE;
                break;
            case PHASE_IDLE:
                /* If our button 4 was pushed advance */
                if(my_btn_changed && my_data.btn == 4 && !their_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_LEADER1:
                /* If their button 2 was pushed advance */
                if(their_btn_changed && their_data.btn == 2 && !my_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_FOLLOW1:
                /* If our button 1 was pushed advance */
                if(my_btn_changed && my_data.btn == 1 && !their_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_LEADER2:
                /* If their button 3 was pushed advance */
                if(their_btn_changed && their_data.btn == 3 && !my_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_FOLLOW2:
                /* If our button 3 was pushed advance */
                if(my_btn_changed && my_data.btn == 3 && !their_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_LEADER3:
                /* If their button 1 was pushed advance */
                if(their_btn_changed && their_data.btn == 1 && !my_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_FOLLOW3:
                /* If our button 3 was pushed advance */
                if(my_btn_changed && my_data.btn == 2 && !their_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            case PHASE_LEADER4:
                /* If their button 1 was pushed advance */
                if(their_btn_changed && their_data.btn == 4 && !my_btn_changed) my_data.phase++;
                else if(my_btn_changed || their_btn_changed) my_data.phase = PHASE_INCORRECT;
                break;
            }

            /* If we went to an incorrect state, calculate our and their LEDs */
            static const int my_remap[] = {1,3,2,0};
            static const int their_remap[] = {2,0,1,3};
            if(my_data.phase == PHASE_INCORRECT)
            {
                /* Always light red LEDs */
                my_data.output |= 1<<4;
                g_output |= 1<<4;
                /* Both of us pressed, or either of us had multiple */
                if(my_data.btn == 255 || their_data.btn == 255 || (my_data.btn && their_data.btn))
                {
                    /* No more specific buttons */
                }
                /* My button */
                else if(my_data.btn)
                {
                    my_data.output |= 1<<my_remap[my_data.btn-1];
                }
                /* Their button */
                else if(their_data.btn)
                {
                    g_output |= 1<<their_remap[their_data.btn-1];
                }
            }
            else
            {
                /* Calculate outputs based on phase */
                switch(my_data.phase)
                {
                    case PHASE_FOLLOW4_COMPLETED:
                        g_output |= 1<<3;
                        /* Fallthrough */
                    case PHASE_LEADER4:
                        my_data.output |= 1<<3;
                        /* Fallthrough */
                    case PHASE_FOLLOW3:
                        g_output |= 1<<2;
                        /* Fallthrough */
                    case PHASE_LEADER3:
                        my_data.output |= 1<<2;
                        /* Fallthrough */
                    case PHASE_FOLLOW2:
                        g_output |= 1<<1;
                        /* Fallthrough */
                    case PHASE_LEADER2:
                        my_data.output |= 1<<1;
                        /* Fallthrough */
                    case PHASE_FOLLOW1:
                        g_output |= 1<<0;
                        /* Fallthrough */
                    case PHASE_LEADER1:
                        my_data.output |= 1<<0;
                }
            }
        }
    }
    /* If we are active 1, we are follower */
    else if(g_rfid_tag_active)
    {
        /* If they are active, output their LEDs */
        if(their_data.active)
        {
            g_output |= their_data.output;
            my_data.phase = PHASE_FOLLOWER;
        }
        else
        {
            /* They are not active, so wait for that */
            my_data.phase = PHASE_INACTIVE;
        }
    }
    /* If we are not active then disable the rest of the indications */
    else
    {  
        /* Nothing to do here */
        my_data.phase = PHASE_INACTIVE;
    }

    /* Send our own state */
    frame_send(&mcast_frame, (const char *)&my_data, sizeof(my_data));

    if(my_data.phase != my_last_phase && g_rfid_tag_active == 2)
    {
        ESP_LOGI(TAG,"Phase changed Master %d -> %d",my_last_phase,my_data.phase);
        my_last_phase = my_data.phase;
    }
    /* If we are in the error state and we are active2, delay 1s for LEDs then reset*/
    if(my_data.phase == PHASE_INCORRECT && g_rfid_tag_active == 2)
    {
        io_output();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        my_data.phase = PHASE_IDLE;
    }
    else if(my_data.phase == PHASE_FOLLOW4_COMPLETED && g_rfid_tag_active == 2)
    {
        /* Blink the LEDs four times */
        for(int i = 0; i < 4; i++)
        {
            g_output = 0x2f;
            my_data.output= 0x2f;
            frame_send(&mcast_frame, (const char *)&my_data, sizeof(my_data));
            io_output();
            vTaskDelay(200 / portTICK_PERIOD_MS);
            g_output = 0x20;
            my_data.output= 0;
            frame_send(&mcast_frame, (const char *)&my_data, sizeof(my_data));
            io_output();
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
        my_data.phase = PHASE_IDLE;
    }

    if(my_data.phase == PHASE_FOLLOWER && my_last_phase != PHASE_FOLLOWER)
    {
        ESP_LOGI(TAG,"Becoming Follower");
    }
    
    /* Periodic state messages */
    static int seq_send_counter = 0;
    seq_send_counter++;
    if(seq_send_counter >= (5000 / FRAME_RATE_MS))
    {
        ESP_LOGI(TAG,"Periodic State: My Phase %2d Buttons %d LEDs %x Active %d",my_data.phase,my_data.btn,g_output,g_rfid_tag_active);
        seq_send_counter = 0;
    }

    /* Store lasts */
    their_last_phase = their_data.phase;
    their_last_btn = their_data.btn;
    my_last_btn = my_data.btn;
    my_last_phase = my_data.phase;
}

/* reset reasons */
static const char * reset_reasons[] = {
    "SP_RST_UNKNOWN",   
    "SP_RST_POWERON",    
    "SP_RST_EXT",        
    "SP_RST_SW",         
    "SP_RST_PANIC",      
    "SP_RST_INT_WDT",    
    "SP_RST_TASK_WDT",   
    "SP_RST_WDT",        
    "SP_RST_DEEPSLEEP",  
    "SP_RST_BROWNOUT",   
    "ESP_RST_SDIO",      
    "ESP_RST_USB",       
    "ESP_RST_JTAG",      
    "ESP_RST_EFUSE",     
    "ESP_RST_PWR_GLITCH",
    "ESP_RST_CPU_LOCKUP",
};


void app_main(void)
{
    ESP_LOGI(TAG, "App Start");
    ESP_LOGI(TAG,"Last Reset Reason: %s",reset_reasons[esp_reset_reason()]);
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
    /* Print reset reason (to netconsole )*/
    ESP_LOGI(TAG,"~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    ESP_LOGI(TAG,"~~~~SYSTEM HAS REBOOTED~~~~~~");
    ESP_LOGI(TAG,"~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    ESP_LOGI(TAG,"Last Reset Reason: %s",reset_reasons[esp_reset_reason()]);
    /* Print hostname (to netconsole) */
    eth_print_hostname();

    /* initialize game logic */
    io_init();
    rfid_init();
    frame_init(&mcast_frame, MULTICAST_GROUP, UDP_PORT);
    
    /* run game logic */
    while (1) {
        io_input();
        game_logic();
        io_output();
        vTaskDelay(FRAME_RATE_MS / portTICK_PERIOD_MS);
    }
}
