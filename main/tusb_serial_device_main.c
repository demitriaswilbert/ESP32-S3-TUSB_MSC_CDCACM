/* USB Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// DESCRIPTION:
// This example contains minimal code to make ESP32-S2 based device
// recognizable by USB-host devices as a USB Serial Device.

#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "esp_partition.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "tusb_cdc_acm.h"

#define BASE_PATH "/usb" // base path to mount the partition
static const char *TAG = "example";
// #define CONFIG_TINYUSB_CDC_RX_BUFSIZE 128
static QueueHandle_t usb_cdc_rx_queue = NULL;
static SemaphoreHandle_t cdc_log_mutex = NULL;
static char log_buf[0x800];
static char format_buf[0x100];

typedef struct {
    uint8_t* buf;
    size_t len;
} buf_len_t;

size_t cdc_log(const char* tag, const char *format, ...)
{   
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;

    if (xSemaphoreTake(cdc_log_mutex, 0xfffffffful) != pdTRUE) 
        return;

    static buf_len_t rx_data;

    snprintf(format_buf, 0x100, "[%s] %s\n", tag, format);

    va_list args;
    va_start(args, format);

    size_t length = vsnprintf(log_buf, 0x800, format_buf, args);
    
    va_end(args);

    for (size_t offset = 0; offset < length; ) {

        size_t remaining = length - offset;
        
        rx_data.len = remaining > CONFIG_TINYUSB_CDC_RX_BUFSIZE? CONFIG_TINYUSB_CDC_RX_BUFSIZE : remaining;
        rx_data.buf = (uint8_t *)malloc(rx_data.len);

        memcpy(rx_data.buf, log_buf + offset, rx_data.len);

        if (xPortInIsrContext()) {
            if (xQueueSendFromISR(usb_cdc_rx_queue, &rx_data, &pxHigherPriorityTaskWoken) != pdTRUE)
                free(rx_data.buf);
        } else if (xQueueSend(usb_cdc_rx_queue, &rx_data, 0xfffffffful) != pdTRUE) {
            free(rx_data.buf);
        }

        offset += rx_data.len;
    }

    xSemaphoreGive(cdc_log_mutex);

    if (pxHigherPriorityTaskWoken) 
        portYIELD_FROM_ISR();

    return length;

}


void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    /* initialization */
    size_t rx_size = 0;
    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;

    static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    static buf_len_t rx_data;

    /* read */
    esp_err_t ret = tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret != ESP_OK) 
        return;
        
    rx_data.len = rx_size;
    rx_data.buf = (uint8_t *)malloc(rx_data.len);

    memcpy(rx_data.buf, buf, rx_size);

    if (xPortInIsrContext()) {
        if (xQueueSendFromISR(usb_cdc_rx_queue, &rx_data, &pxHigherPriorityTaskWoken) != pdTRUE)
            free(rx_data.buf);
        if (pxHigherPriorityTaskWoken) 
            portYIELD_FROM_ISR();
    } else if (xQueueSend(usb_cdc_rx_queue, &rx_data, 0xfffffffful) != pdTRUE) {
        free(rx_data.buf);
    }

}

void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rst = event->line_state_changed_data.rts;
    //ESP_LOGI(TAG, "Line state changed! dtr:%d, rst:%d", dtr, rst);
}

void cdc_process_task(void* param) {
    (void)param;
    buf_len_t rx_data;

    vTaskDelay(8000);

    while (true) {
        if (xQueueReceive(usb_cdc_rx_queue, &rx_data, 0xfffffffful) == pdTRUE) {
            tinyusb_cdcacm_write_queue(0, rx_data.buf, rx_data.len);
            tinyusb_cdcacm_write_flush(0, 0);
            free(rx_data.buf);
        }
    }
    vTaskDelete(NULL);
}

void background_task(void* param) {

    while (true) {
        vTaskDelay(1000);
        cdc_log("BG", "Hello World %s", "dewe");
    }
    vTaskDelete(NULL);
}

static bool file_exists(const char *file_path)
{
    struct stat buffer;
    return stat(file_path, &buffer) == 0;
}

static void file_operations(void)
{
    const char *directory = "/usb/esp";
    const char *file_path = "/usb/esp/test.txt";

    struct stat s = {0};
    bool directory_exists = stat(directory, &s) == 0;
    if (!directory_exists) {
        if (mkdir(directory, 0775) != 0) {
            cdc_log(TAG, "mkdir failed with errno: %s", strerror(errno));
        }
    }

    if (!file_exists(file_path)) {
        cdc_log(TAG, "Creating file");
        FILE *f = fopen(file_path, "w");
        if (f == NULL) {
            cdc_log(TAG, "Failed to open file for writing");
            return;
        }
        fprintf(f, "Hello Demitrias Wilbert World!\n");
        fclose(f);
    }

    FILE *f;
    cdc_log(TAG, "Reading file");
    f = fopen(file_path, "r");
    if (f == NULL) {
        cdc_log(TAG, "Failed to open file for reading");
        return;
    }
    char line[64];
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    cdc_log(TAG, "Read from file: '%s'", line);
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    cdc_log(TAG, "Initializing wear levelling");

    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        cdc_log(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }

    return wl_mount(data_partition, wl_handle);
}

void app_main(void)
{
    cdc_log_mutex = xSemaphoreCreateMutex();
    usb_cdc_rx_queue = xQueueCreate(0x4000, sizeof(buf_len_t));
    
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = wl_handle
    };
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&config_spi));
    ESP_ERROR_CHECK(tinyusb_msc_storage_mount(BASE_PATH));
    file_operations();

    cdc_log(TAG, "USB initialization");

    tinyusb_config_t tusb_cfg = {}; // the configuration using default values
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = CONFIG_TINYUSB_CDC_RX_BUFSIZE,
        .callback_rx = &tinyusb_cdc_rx_callback, // the first way to register a callback
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };
    
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
    /* the second way to register a callback */
    ESP_ERROR_CHECK(tinyusb_cdcacm_register_callback(
                        TINYUSB_CDC_ACM_0,
                        CDC_EVENT_LINE_STATE_CHANGED,
                        &tinyusb_cdc_line_state_changed_callback));
    cdc_log(TAG, "USB initialization DONE");

    xTaskCreate(cdc_process_task, "cdc_task", 4096, NULL, 4, NULL);

    xTaskCreate(background_task, "bg_task", 4096, NULL, tskIDLE_PRIORITY, NULL);

}
