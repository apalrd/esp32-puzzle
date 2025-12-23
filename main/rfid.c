#include <esp_log.h>
#include "rc522.h"
#include "rc522_picc.h"

//Choose one of these
#define USE_SPI
//#define USE_I2C

#if defined USE_SPI && defined USE_I2C
#error "Define only SPI or I2C but not both"
#endif

static const char *TAG = "rfid";

#ifdef USE_I2C
#include "driver/rc522_i2c.h"
#define RC522_I2C_ADDRESS      (0x28)
#define RC522_I2C_GPIO_SDA     (32)
#define RC522_I2C_GPIO_SCL     (33)
#define RC522_SCANNER_GPIO_RST (20) // -1 for soft-reset

static rc522_i2c_config_t driver_config = {
    .port = I2C_NUM_1,
    .device_address = RC522_I2C_ADDRESS,
    .rw_timeout_ms = 1000,
    .config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RC522_I2C_GPIO_SDA,
        .scl_io_num = RC522_I2C_GPIO_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    },
    .rst_io_num = RC522_SCANNER_GPIO_RST,
};
#endif // USE_I2C

#ifdef USE_SPI
#include "driver/rc522_spi.h"

#define RC522_SPI_BUS_GPIO_MISO    (34)
#define RC522_SPI_BUS_GPIO_MOSI    (20)
#define RC522_SPI_BUS_GPIO_SCLK    (4)
#define RC522_SPI_SCANNER_GPIO_SDA (2)
#define RC522_SCANNER_GPIO_RST     (-1) // soft-reset

static rc522_spi_config_t driver_config = {
    .host_id = SPI3_HOST,
    .bus_config = &(spi_bus_config_t){
        .miso_io_num = RC522_SPI_BUS_GPIO_MISO,
        .mosi_io_num = RC522_SPI_BUS_GPIO_MOSI,
        .sclk_io_num = RC522_SPI_BUS_GPIO_SCLK,
    },
    .dev_config = {
        .spics_io_num = RC522_SPI_SCANNER_GPIO_SDA,
    },
    .rst_io_num = RC522_SCANNER_GPIO_RST,
};
#endif

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;
//global rfid tag received
int g_rfid_tag_active = 0;

static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    static const char * event_names[] = {
    "RC522_PICC_STATE_IDLE",
    "RC522_PICC_STATE_READY",
    "RC522_PICC_STATE_ACTIVE",
    "RC522_PICC_STATE_HALT",
    "RC522_PICC_STATE_READY_H",
    "RC522_PICC_STATE_ACTIVE_H",
    "RC522_PICC_STATE_AUTHENTICATED",
    };
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    ESP_LOGD(TAG,"Got PICC Event %s",event_names[event->picc->state]);


    if (event->picc->state != RC522_PICC_STATE_ACTIVE) {
        ESP_LOGI(TAG,"Lost Tag");
        g_rfid_tag_active = 0;
        return;
    }

    ESP_LOGI(TAG,"Got Tag %02x %02x %02x %02x",
        event->picc->uid.value[0],
        event->picc->uid.value[1],
        event->picc->uid.value[2],
        event->picc->uid.value[3]);
    g_rfid_tag_active = 1;

    /* Check to see if this is card id 2, otherwise assume id 1 */
    static const uint8_t good_card[] = {0xA1, 0x37, 0x78, 0x7B};
    if(memcmp(event->picc->uid.value,good_card,4) == 0)
    {
        g_rfid_tag_active = 2;
        ESP_LOGI(TAG,"Got Well-Known Card");
    }
}

#define RFID_ERROR_CHECK(err) rfid_error_check_int(err,__FUNCTION__,__LINE__)
static void rfid_error_check_int(const esp_err_t err,const char * func, const int line)
{
    if(err != ESP_OK) {
        ESP_LOGE(TAG,"Got error %x at %s line %d",err,func,line);
    }
}

void rfid_init()
{
    ESP_LOGI(TAG,"Initializing RFID RC522");
#ifdef USE_I2C
    ESP_LOGI(TAG,"Have %d i2c busses",I2C_NUM_MAX);
    RFID_ERROR_CHECK(rc522_i2c_create(&driver_config, &driver));
    ESP_LOGI(TAG,"RC522 I2C driver created");
#endif //USE_I2C
#ifdef USE_SPI
    rc522_spi_create(&driver_config, &driver);
    ESP_LOGI(TAG,"RC522 SPI driver created");
#endif //USE_SPI
    RFID_ERROR_CHECK(rc522_driver_install(driver));
    ESP_LOGI(TAG,"RC522 driver installed");

    rc522_config_t scanner_config = {
        .driver = driver,
    };

    RFID_ERROR_CHECK(rc522_create(&scanner_config, &scanner));
    ESP_LOGI(TAG,"RC522 driver created");
    RFID_ERROR_CHECK(rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL));
    ESP_LOGI(TAG,"RC522 event registered");
    RFID_ERROR_CHECK(rc522_start(scanner));
    ESP_LOGI(TAG,"RC522 complete");
}
