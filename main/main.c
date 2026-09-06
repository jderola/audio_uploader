/*
 * ESP32-S3 USB WAV -> n8n Audio Diary Uploader
 *
 * ESP-IDF:
 *   v5.5.5
 *
 * Hardware:
 *   GPIO19 = USB D-
 *   GPIO20 = USB D+
 *   USB VBUS = external regulated 5V
 *   USB GND  = ESP32 GND
 *
 * Operation:
 *
 *   USB flash drive
 *          |
 *          v
 *   USB Host MSC
 *          |
 *          v
 *   FAT filesystem
 *          |
 *          v
 *   Find newest WAV
 *          |
 *          v
 *   4096-byte streaming
 *          |
 *          v
 *   HTTPS multipart POST
 *          |
 *          v
 *   n8n webhook
 *
 * No ESP-Claw.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"


/* ============================================================
 * USER CONFIGURATION
 * ============================================================ */

#define WIFI_SSID       "Virus"
#define WIFI_PASSWORD   "abcd1234@358x"

#define N8N_WEBHOOK_URL \
    "https://derola.app.n8n.cloud/webhook/audio-diary"

#define USB_MOUNT_BASE "/usb"

#define STREAM_BUFFER_SIZE 4096

#define SCAN_INTERVAL_MS          5000
#define WIFI_RETRY_DELAY_MS      10000
#define UPLOAD_RETRY_DELAY_MS    10000

#define HTTP_TIMEOUT_MS          60000

#define WIFI_CONNECTED_BIT BIT0

#define MAX_FILENAME_LEN 128

/*
 * We support one flash drive.
 *
 * This makes the application simpler and avoids multiple
 * simultaneous upload sources.
 */
#define MAX_MSC_DEVICES 1


static const char *TAG = "AUDIO_UPLOADER";


/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

static EventGroupHandle_t wifi_event_group;

static QueueHandle_t app_queue;

static volatile bool usb_ready = false;
static volatile bool upload_active = false;

static char active_mount_path[32] = "";


/* ============================================================
 * USB APPLICATION MESSAGE
 * ============================================================ */

typedef enum {

    APP_DEVICE_CONNECTED,

    APP_DEVICE_DISCONNECTED

} app_message_id_t;


typedef struct {

    app_message_id_t id;

    union {

        uint8_t usb_address;

        msc_host_device_handle_t device_handle;

    } data;

} app_message_t;


/* ============================================================
 * MSC DEVICE
 * ============================================================ */

typedef struct {

    uint8_t usb_addr;

    msc_host_device_handle_t msc_device;

    msc_host_vfs_handle_t vfs_handle;

} msc_device_entry_t;


static msc_device_entry_t *
    msc_devices[MAX_MSC_DEVICES] = {0};


/* ============================================================
 * FIND FREE MSC SLOT
 * ============================================================ */

static int find_free_msc_slot(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {

        if (msc_devices[i] == NULL) {

            return i;
        }
    }

    return -1;
}


/* ============================================================
 * FIND SLOT BY HANDLE
 * ============================================================ */

static int find_msc_slot_by_handle(
    msc_host_device_handle_t handle
)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {

        if (
            msc_devices[i] != NULL &&
            msc_devices[i]->msc_device == handle
        ) {

            return i;
        }
    }

    return -1;
}


/* ============================================================
 * MSC EVENT CALLBACK
 * ============================================================ */

static void msc_event_callback(
    const msc_host_event_t *event,
    void *arg
)
{
    if (event == NULL) {

        return;
    }


    if (
        event->event ==
        MSC_DEVICE_CONNECTED
    ) {

        ESP_LOGI(
            TAG,
            "USB MSC connected, address=%d",
            event->device.address
        );


        app_message_t message = {

            .id =
                APP_DEVICE_CONNECTED,

            .data.usb_address =
                event->device.address
        };


        /*
         * Callback executes in MSC context.
         * Send notification to application task.
         */

        xQueueSend(
            app_queue,
            &message,
            portMAX_DELAY
        );
    }


    else if (
        event->event ==
        MSC_DEVICE_DISCONNECTED
    ) {

        ESP_LOGW(
            TAG,
            "USB MSC disconnected"
        );


        app_message_t message = {

            .id =
                APP_DEVICE_DISCONNECTED,

            .data.device_handle =
                event->device.handle
        };


        xQueueSend(
            app_queue,
            &message,
            portMAX_DELAY
        );
    }


    else {

        ESP_LOGW(
            TAG,
            "Unsupported MSC event=%d",
            event->event
        );
    }
}


/* ============================================================
 * MOUNT MSC DEVICE
 * ============================================================ */

static esp_err_t mount_msc_device(
    uint8_t usb_address
)
{
    int slot =
        find_free_msc_slot();


    if (slot < 0) {

        ESP_LOGE(
            TAG,
            "No free MSC slot"
        );

        return ESP_ERR_NO_MEM;
    }


    msc_devices[slot] =
        calloc(
            1,
            sizeof(msc_device_entry_t)
        );


    if (
        msc_devices[slot] == NULL
    ) {

        ESP_LOGE(
            TAG,
            "MSC allocation failed"
        );

        return ESP_ERR_NO_MEM;
    }


    esp_err_t err =
        msc_host_install_device(
            usb_address,
            &msc_devices[slot]->msc_device
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "msc_host_install_device: %s",
            esp_err_to_name(err)
        );

        free(
            msc_devices[slot]
        );

        msc_devices[slot] = NULL;

        return err;
    }


    msc_devices[slot]->usb_addr =
        usb_address;


    /*
     * FAT mount configuration.
     *
     * Do NOT format the USB drive automatically.
     */

    const esp_vfs_fat_mount_config_t mount_config = {

        .format_if_mount_failed = false,

        .max_files = 4,

        .allocation_unit_size = 8192
    };


    char mount_path[32];


    snprintf(
        mount_path,
        sizeof(mount_path),
        USB_MOUNT_BASE "%d",
        slot
    );


    ESP_LOGI(
        TAG,
        "Mounting USB filesystem at %s",
        mount_path
    );


    err =
        msc_host_vfs_register(

            msc_devices[slot]->msc_device,

            mount_path,

            &mount_config,

            &msc_devices[slot]->vfs_handle
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "msc_host_vfs_register: %s",
            esp_err_to_name(err)
        );


        msc_host_uninstall_device(
            msc_devices[slot]->msc_device
        );


        free(
            msc_devices[slot]
        );

        msc_devices[slot] = NULL;

        return err;
    }


    strncpy(
        active_mount_path,
        mount_path,
        sizeof(active_mount_path) - 1
    );


    active_mount_path[
        sizeof(active_mount_path) - 1
    ] = '\0';


    usb_ready = true;


    ESP_LOGI(
        TAG,
        "USB filesystem mounted: %s",
        active_mount_path
    );


    /*
     * Print USB device information.
     */

    msc_host_device_info_t info;


    err =
        msc_host_get_device_info(
            msc_devices[slot]->msc_device,
            &info
        );


    if (
        err == ESP_OK
    ) {

        uint64_t capacity_mb =

            (
                (uint64_t)
                info.sector_size *

                info.sector_count
            )
            /
            (1024ULL * 1024ULL);


        ESP_LOGI(
            TAG,
            "USB capacity: %" PRIu64 " MB",
            capacity_mb
        );


        ESP_LOGI(
            TAG,
            "Sector size: %" PRIu32,
            info.sector_size
        );


        ESP_LOGI(
            TAG,
            "Sector count: %" PRIu32,
            info.sector_count
        );


        ESP_LOGI(
            TAG,
            "VID=0x%04X PID=0x%04X",
            info.idVendor,
            info.idProduct
        );
    }


    return ESP_OK;
}


/* ============================================================
 * UNMOUNT MSC DEVICE
 * ============================================================ */

static void unmount_msc_device(
    msc_host_device_handle_t handle
)
{
    int slot =
        find_msc_slot_by_handle(handle);


    if (slot < 0) {

        ESP_LOGW(
            TAG,
            "Disconnected MSC handle not found"
        );

        usb_ready = false;

        active_mount_path[0] = '\0';

        return;
    }


    ESP_LOGW(
        TAG,
        "Unmounting USB device"
    );


    usb_ready = false;


    active_mount_path[0] = '\0';


    if (
        msc_devices[slot]->vfs_handle
    ) {

        esp_err_t err =
            msc_host_vfs_unregister(
                msc_devices[slot]->vfs_handle
            );


        if (
            err != ESP_OK
        ) {

            ESP_LOGW(
                TAG,
                "VFS unregister: %s",
                esp_err_to_name(err)
            );
        }


        msc_devices[slot]->vfs_handle =
            NULL;
    }


    if (
        msc_devices[slot]->msc_device
    ) {

        esp_err_t err =
            msc_host_uninstall_device(
                msc_devices[slot]->msc_device
            );


        if (
            err != ESP_OK
        ) {

            ESP_LOGW(
                TAG,
                "MSC uninstall: %s",
                esp_err_to_name(err)
            );
        }
    }


    free(
        msc_devices[slot]
    );


    msc_devices[slot] = NULL;


    ESP_LOGI(
        TAG,
        "USB device removed cleanly"
    );
}


/* ============================================================
 * USB HOST TASK
 * ============================================================ */

static void usb_host_task(
    void *arg
)
{
    ESP_LOGI(
        TAG,
        "Starting USB Host"
    );


    const usb_host_config_t host_config = {

        .intr_flags =
            ESP_INTR_FLAG_LOWMED
    };


    esp_err_t err =
        usb_host_install(
            &host_config
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "usb_host_install failed: %s",
            esp_err_to_name(err)
        );

        vTaskDelete(NULL);

        return;
    }


    /*
     * ESP-IDF's MSC component uses this configuration.
     */

    const msc_host_driver_config_t msc_config = {

        /*
         * Spelling is intentional:
         * this is the field used by the
         * ESP-IDF MSC example.
         */
        .create_backround_task = true,

        .task_priority = 5,

        .stack_size = 4096,

        .callback =
            msc_event_callback
    };


    err =
        msc_host_install(
            &msc_config
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "msc_host_install failed: %s",
            esp_err_to_name(err)
        );


        usb_host_uninstall();

        vTaskDelete(NULL);

        return;
    }


    ESP_LOGI(
        TAG,
        "USB Host MSC ready"
    );


    while (true) {

        uint32_t event_flags = 0;


        err =
            usb_host_lib_handle_events(
                portMAX_DELAY,
                &event_flags
            );


        if (
            err != ESP_OK
        ) {

            ESP_LOGW(
                TAG,
                "USB host event error: %s",
                esp_err_to_name(err)
            );
        }


        /*
         * Normally the application stays alive indefinitely.
         *
         * Device removal is handled through MSC callback.
         */
    }
}


/* ============================================================
 * WIFI EVENT HANDLER
 * ============================================================ */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START
    ) {

        ESP_LOGI(
            TAG,
            "Wi-Fi started"
        );


        esp_wifi_connect();
    }


    else if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {

        xEventGroupClearBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );


        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected; reconnecting"
        );


        /*
         * Non-blocking reconnect.
         */

        esp_wifi_connect();
    }


    else if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;


        ESP_LOGI(
            TAG,
            "Wi-Fi connected: "
            IPSTR,
            IP2STR(
                &event->ip_info.ip
            )
        );


        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}


/* ============================================================
 * WIFI INITIALIZATION
 * ============================================================ */

static void wifi_init(void)
{
    wifi_event_group =
        xEventGroupCreate();


    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );


    esp_netif_create_default_wifi_sta();


    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();


    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );


    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        )
    );


    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL
        )
    );


    wifi_config_t config = {0};


    strncpy(
        (char *)config.sta.ssid,
        WIFI_SSID,
        sizeof(config.sta.ssid)
    );


    strncpy(
        (char *)config.sta.password,
        WIFI_PASSWORD,
        sizeof(config.sta.password)
    );


    config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;


    config.sta.pmf_cfg.capable = true;

    config.sta.pmf_cfg.required = false;


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &config
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    ESP_LOGI(
        TAG,
        "Connecting Wi-Fi: %s",
        WIFI_SSID
    );
}


/* ============================================================
 * WIFI STATUS
 * ============================================================ */

static bool wifi_connected(void)
{
    return (

        xEventGroupGetBits(
            wifi_event_group
        )
        &
        WIFI_CONNECTED_BIT
    );
}


/* ============================================================
 * NVS STATE
 * ============================================================ */

#define NVS_NAMESPACE "audio_state"

#define NVS_FILENAME  "filename"
#define NVS_MTIME     "mtime"
#define NVS_SIZE      "size"


typedef struct {

    char filename[MAX_FILENAME_LEN];

    int64_t mtime;

    int64_t size;

    bool valid;

} upload_state_t;


/* ------------------------------------------------------------
 * LOAD STATE
 * ------------------------------------------------------------ */

static void load_upload_state(
    upload_state_t *state
)
{
    memset(
        state,
        0,
        sizeof(*state)
    );


    nvs_handle_t nvs;


    esp_err_t err =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READONLY,
            &nvs
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGI(
            TAG,
            "No previous upload state"
        );

        return;
    }


    size_t len =
        sizeof(state->filename);


    err =
        nvs_get_str(
            nvs,
            NVS_FILENAME,
            state->filename,
            &len
        );


    if (
        err != ESP_OK
    ) {

        nvs_close(nvs);

        return;
    }


    if (
        nvs_get_i64(
            nvs,
            NVS_MTIME,
            &state->mtime
        ) != ESP_OK
    ) {

        nvs_close(nvs);

        return;
    }


    if (
        nvs_get_i64(
            nvs,
            NVS_SIZE,
            &state->size
        ) != ESP_OK
    ) {

        nvs_close(nvs);

        return;
    }


    state->valid = true;


    nvs_close(nvs);


    ESP_LOGI(
        TAG,
        "Last uploaded: %s",
        state->filename
    );
}


/* ------------------------------------------------------------
 * SAVE STATE
 * ------------------------------------------------------------ */

static esp_err_t save_upload_state(
    const char *filename,
    int64_t mtime,
    int64_t size
)
{
    nvs_handle_t nvs;


    esp_err_t err =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &nvs
        );


    if (
        err != ESP_OK
    ) {

        return err;
    }


    err =
        nvs_set_str(
            nvs,
            NVS_FILENAME,
            filename
        );


    if (
        err == ESP_OK
    ) {

        err =
            nvs_set_i64(
                nvs,
                NVS_MTIME,
                mtime
            );
    }


    if (
        err == ESP_OK
    ) {

        err =
            nvs_set_i64(
                nvs,
                NVS_SIZE,
                size
            );
    }


    if (
        err == ESP_OK
    ) {

        err =
            nvs_commit(nvs);
    }


    nvs_close(nvs);


    return err;
}


/* ============================================================
 * WAV FILE INFORMATION
 * ============================================================ */

typedef struct {

    char path[256];

    char filename[MAX_FILENAME_LEN];

    int64_t mtime;

    int64_t size;

    bool found;

} wav_file_t;


/* ============================================================
 * WAV EXTENSION CHECK
 * ============================================================ */

static bool is_wav_file(
    const char *name
)
{
    if (
        name == NULL
    ) {

        return false;
    }


    size_t len =
        strlen(name);


    if (
        len < 4
    ) {

        return false;
    }


    return (
        strcasecmp(
            name + len - 4,
            ".wav"
        ) == 0
    );
}


/* ============================================================
 * FIND NEWEST WAV IN ROOT
 * ============================================================ */

static bool find_newest_wav(
    const char *mount_path,
    wav_file_t *result
)
{
    memset(
        result,
        0,
        sizeof(*result)
    );


    DIR *dir =
        opendir(mount_path);


    if (
        dir == NULL
    ) {

        ESP_LOGE(
            TAG,
            "Cannot open USB root %s: %s",
            mount_path,
            strerror(errno)
        );

        return false;
    }


    struct dirent *entry;


    while (
        (entry = readdir(dir))
        != NULL
    ) {

        if (
            !is_wav_file(
                entry->d_name
            )
        ) {

            continue;
        }


        char path[256];


        snprintf(
            path,
            sizeof(path),
            "%s/%s",
            mount_path,
            entry->d_name
        );


        struct stat st;


        if (
            stat(
                path,
                &st
            ) != 0
        ) {

            ESP_LOGW(
                TAG,
                "stat failed: %s",
                path
            );

            continue;
        }


        if (
            !S_ISREG(st.st_mode)
        ) {

            continue;
        }


        ESP_LOGI(
            TAG,
            "WAV found: %s size=%" PRId64
            " mtime=%" PRId64,
            entry->d_name,
            (int64_t)st.st_size,
            (int64_t)st.st_mtime
        );


        /*
         * Newest modification time wins.
         *
         * If timestamps are identical, larger file wins.
         */

        if (
            !result->found ||

            st.st_mtime >
                result->mtime ||

            (
                st.st_mtime ==
                    result->mtime &&

                st.st_size >
                    result->size
            )
        ) {

            result->found = true;

            result->mtime =
                (int64_t)st.st_mtime;

            result->size =
                (int64_t)st.st_size;


            strncpy(
                result->filename,
                entry->d_name,
                sizeof(result->filename) - 1
            );


            strncpy(
                result->path,
                path,
                sizeof(result->path) - 1
            );
        }
    }


    closedir(dir);


    return result->found;
}


/* ============================================================
 * HTTP WRITE ALL
 * ============================================================ */

static bool http_write_all(
    esp_http_client_handle_t client,
    const uint8_t *data,
    size_t length
)
{
    size_t offset = 0;


    while (
        offset < length
    ) {

        int written =
            esp_http_client_write(
                client,
                (const char *)(data + offset),
                length - offset
            );


        if (
            written <= 0
        ) {

            ESP_LOGE(
                TAG,
                "HTTP write failed"
            );

            return false;
        }


        offset += written;
    }


    return true;
}


/* ============================================================
 * UPLOAD WAV
 * ============================================================ */

static bool upload_wav(
    const wav_file_t *wav
)
{
    if (
        !wifi_connected()
    ) {

        ESP_LOGW(
            TAG,
            "Upload skipped: Wi-Fi unavailable"
        );

        return false;
    }


    FILE *file =
        fopen(
            wav->path,
            "rb"
        );


    if (
        file == NULL
    ) {

        ESP_LOGE(
            TAG,
            "Cannot open %s",
            wav->path
        );

        return false;
    }


    /*
     * Multipart boundary.
     */

    const char *boundary =
        "----ESP32S3AudioDiaryBoundary7A91";


    char header[512];


    int header_len =
        snprintf(

            header,
            sizeof(header),

            "--%s\r\n"
            "Content-Disposition: form-data; "
            "name=\"file\"; "
            "filename=\"%s\"\r\n"
            "Content-Type: audio/wav\r\n"
            "\r\n",

            boundary,
            wav->filename
        );


    if (
        header_len <= 0 ||
        header_len >= sizeof(header)
    ) {

        fclose(file);

        return false;
    }


    char footer[128];


    int footer_len =
        snprintf(

            footer,
            sizeof(footer),

            "\r\n--%s--\r\n",

            boundary
        );


    if (
        footer_len <= 0
    ) {

        fclose(file);

        return false;
    }


    /*
     * Exact Content-Length.
     */

    int64_t content_length =

        (int64_t)header_len +

        wav->size +

        footer_len;


    ESP_LOGI(
        TAG,
        "Uploading: %s",
        wav->filename
    );


    ESP_LOGI(
        TAG,
        "File size: %" PRId64 " bytes",
        wav->size
    );


    ESP_LOGI(
        TAG,
        "HTTP Content-Length: %" PRId64,
        content_length
    );


    esp_http_client_config_t config = {

        .url =
            N8N_WEBHOOK_URL,

        .method =
            HTTP_METHOD_POST,

        .timeout_ms =
            HTTP_TIMEOUT_MS,

        .crt_bundle_attach =
            esp_crt_bundle_attach,

        .keep_alive_enable =
            true
    };


    esp_http_client_handle_t client =
        esp_http_client_init(
            &config
        );


    if (
        client == NULL
    ) {

        fclose(file);

        return false;
    }


    char content_type[128];


    snprintf(
        content_type,
        sizeof(content_type),
        "multipart/form-data; boundary=%s",
        boundary
    );


    esp_err_t err =
        esp_http_client_set_header(
            client,
            "Content-Type",
            content_type
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "Failed setting Content-Type"
        );

        esp_http_client_cleanup(
            client
        );

        fclose(file);

        return false;
    }


    /*
     * Open HTTP request with known length.
     *
     * This causes ESP-IDF to send a proper
     * Content-Length header.
     */

    err =
        esp_http_client_open(
            client,
            content_length
        );


    if (
        err != ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "HTTP open failed: %s",
            esp_err_to_name(err)
        );

        esp_http_client_cleanup(
            client
        );

        fclose(file);

        return false;
    }


    /*
     * Multipart header.
     */

    if (
        !http_write_all(
            client,
            (const uint8_t *)header,
            header_len
        )
    ) {

        goto upload_error;
    }


    /*
     * 4096-byte streaming buffer.
     *
     * The complete WAV is NEVER loaded into RAM.
     */

    uint8_t *buffer =
        malloc(
            STREAM_BUFFER_SIZE
        );


    if (
        buffer == NULL
    ) {

        ESP_LOGE(
            TAG,
            "4096-byte buffer allocation failed"
        );

        goto upload_error;
    }


    int64_t total_sent = 0;

    uint32_t chunks = 0;


    while (true) {

        if (
            !wifi_connected()
        ) {

            ESP_LOGE(
                TAG,
                "Wi-Fi lost during upload"
            );

            free(buffer);

            goto upload_error;
        }


        size_t bytes_read =
            fread(
                buffer,
                1,
                STREAM_BUFFER_SIZE,
                file
            );


        if (
            bytes_read == 0
        ) {

            if (
                feof(file)
            ) {

                break;
            }


            ESP_LOGE(
                TAG,
                "USB file read error"
            );


            free(buffer);

            goto upload_error;
        }


        if (
            !http_write_all(
                client,
                buffer,
                bytes_read
            )
        ) {

            free(buffer);

            goto upload_error;
        }


        total_sent +=
            bytes_read;


        chunks++;


        /*
         * Log every 256 KB.
         */

        if (
            chunks % 64 == 0 ||
            total_sent == wav->size
        ) {

            int percent = 0;


            if (
                wav->size > 0
            ) {

                percent =
                    (int)(
                        (
                            total_sent * 100
                        )
                        /
                        wav->size
                    );
            }


            ESP_LOGI(
                TAG,
                "Progress: %" PRId64
                "/%" PRId64
                " (%d%%), chunks=%" PRIu32,

                total_sent,
                wav->size,
                percent,
                chunks
            );
        }
    }


    free(buffer);


    /*
     * Multipart footer.
     */

    if (
        !http_write_all(
            client,
            (const uint8_t *)footer,
            footer_len
        )
    ) {

        goto upload_error;
    }


    fclose(file);


    ESP_LOGI(
        TAG,
        "WAV payload transmitted"
    );


    /*
     * Receive HTTP response.
     */

    int response_status =
        esp_http_client_fetch_headers(
            client
        );


    if (
        response_status < 0
    ) {

        ESP_LOGE(
            TAG,
            "HTTP response failed"
        );

        goto response_error;
    }


    int status =
        esp_http_client_get_status_code(
            client
        );


    ESP_LOGI(
        TAG,
        "n8n HTTP response: %d",
        status
    );


    /*
     * Read small response body.
     */

    char response[512];


    int response_len =
        esp_http_client_read_response(
            client,
            response,
            sizeof(response) - 1
        );


    if (
        response_len > 0
    ) {

        response[response_len] =
            '\0';


        ESP_LOGI(
            TAG,
            "n8n response: %s",
            response
        );
    }


    esp_http_client_close(
        client
    );


    esp_http_client_cleanup(
        client
    );


    if (
        status >= 200 &&
        status < 300
    ) {

        ESP_LOGI(
            TAG,
            "UPLOAD SUCCESS"
        );

        return true;
    }


    ESP_LOGE(
        TAG,
        "UPLOAD FAILED: HTTP %d",
        status
    );


    return false;


upload_error:

    fclose(file);


response_error:

    esp_http_client_close(
        client
    );

    esp_http_client_cleanup(
        client
    );


    return false;
}


/* ============================================================
 * AUDIO PROCESSOR TASK
 * ============================================================ */

static void audio_task(
    void *arg
)
{
    upload_state_t state;


    load_upload_state(
        &state
    );


    while (true) {

        /*
         * USB must be mounted.
         */

        if (
            !usb_ready
        ) {

            vTaskDelay(
                pdMS_TO_TICKS(
                    SCAN_INTERVAL_MS
                )
            );

            continue;
        }


        /*
         * Wi-Fi must be available.
         */

        if (
            !wifi_connected()
        ) {

            ESP_LOGW(
                TAG,
                "Waiting for Wi-Fi..."
            );


            vTaskDelay(
                pdMS_TO_TICKS(
                    WIFI_RETRY_DELAY_MS
                )
            );


            continue;
        }


        /*
         * Find newest WAV.
         */

        wav_file_t newest;


        if (
            !find_newest_wav(
                active_mount_path,
                &newest
            )
        ) {

            ESP_LOGI(
                TAG,
                "No WAV files found"
            );


            vTaskDelay(
                pdMS_TO_TICKS(
                    SCAN_INTERVAL_MS
                )
            );


            continue;
        }


        ESP_LOGI(
            TAG,
            "Newest WAV: %s",
            newest.filename
        );


        /*
         * Deduplication.
         *
         * Filename + timestamp + size.
         *
         * This also allows a file to be uploaded again
         * if it is replaced/modified.
         */

        if (
            state.valid &&

            strcmp(
                state.filename,
                newest.filename
            ) == 0 &&

            state.mtime ==
                newest.mtime &&

            state.size ==
                newest.size
        ) {

            vTaskDelay(
                pdMS_TO_TICKS(
                    SCAN_INTERVAL_MS
                )
            );


            continue;
        }


        if (
            upload_active
        ) {

            vTaskDelay(
                pdMS_TO_TICKS(1000)
            );

            continue;
        }


        upload_active = true;


        bool success =
            upload_wav(
                &newest
            );


        upload_active = false;


        if (
            success
        ) {

            esp_err_t err =
                save_upload_state(

                    newest.filename,

                    newest.mtime,

                    newest.size
                );


            if (
                err == ESP_OK
            ) {

                memset(
                    &state,
                    0,
                    sizeof(state)
                );


                strncpy(
                    state.filename,
                    newest.filename,
                    sizeof(state.filename) - 1
                );


                state.mtime =
                    newest.mtime;


                state.size =
                    newest.size;


                state.valid = true;


                ESP_LOGI(
                    TAG,
                    "Upload state saved"
                );
            }

            else {

                ESP_LOGE(
                    TAG,
                    "NVS save failed: %s",
                    esp_err_to_name(err)
                );
            }
        }

        else {

            ESP_LOGW(
                TAG,
                "Upload failed; retrying later"
            );


            vTaskDelay(
                pdMS_TO_TICKS(
                    UPLOAD_RETRY_DELAY_MS
                )
            );
        }


        vTaskDelay(
            pdMS_TO_TICKS(
                SCAN_INTERVAL_MS
            )
        );
    }
}


/* ============================================================
 * USB APPLICATION EVENT TASK
 * ============================================================ */

static void usb_application_task(
    void *arg
)
{
    while (true) {

        app_message_t message;


        if (
            xQueueReceive(
                app_queue,
                &message,
                portMAX_DELAY
            )
            != pdTRUE
        ) {

            continue;
        }


        if (
            message.id ==
            APP_DEVICE_CONNECTED
        ) {

            ESP_LOGI(
                TAG,
                "Processing USB insertion"
            );


            /*
             * Wait a short time for the drive's
             * descriptors/media to settle.
             */

            vTaskDelay(
                pdMS_TO_TICKS(250)
            );


            esp_err_t err =
                mount_msc_device(
                    message.data.usb_address
                );


            if (
                err != ESP_OK
            ) {

                ESP_LOGE(
                    TAG,
                    "USB mount failed: %s",
                    esp_err_to_name(err)
                );
            }
        }


        else if (
            message.id ==
            APP_DEVICE_DISCONNECTED
        ) {

            unmount_msc_device(
                message.data.device_handle
            );
        }
    }
}


/* ============================================================
 * NVS INITIALIZATION
 * ============================================================ */

static void nvs_init(void)
{
    esp_err_t err =
        nvs_flash_init();


    if (
        err ==
        ESP_ERR_NVS_NO_FREE_PAGES ||

        err ==
        ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {

        ESP_LOGW(
            TAG,
            "Erasing/reinitializing NVS"
        );


        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );


        err =
            nvs_flash_init();
    }


    ESP_ERROR_CHECK(err);
}


/* ============================================================
 * APPLICATION ENTRY
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "=========================================="
    );


    ESP_LOGI(
        TAG,
        "ESP32-S3 AUDIO DIARY UPLOADER"
    );


    ESP_LOGI(
        TAG,
        "ESP-Claw REPLACED"
    );


    ESP_LOGI(
        TAG,
        "USB MSC -> WAV -> n8n"
    );


    ESP_LOGI(
        TAG,
        "=========================================="
    );


    /*
     * NVS
     */

    nvs_init();


    /*
     * Queue for USB events.
     */

    app_queue =
        xQueueCreate(
            8,
            sizeof(app_message_t)
        );


    if (
        app_queue == NULL
    ) {

        ESP_LOGE(
            TAG,
            "Cannot create USB queue"
        );

        abort();
    }


    /*
     * Wi-Fi.
     */

    wifi_init();


    /*
     * USB Host.
     */

    BaseType_t usb_task_result =

        xTaskCreate(
            usb_host_task,
            "usb_host",
            8192,
            NULL,
            6,
            NULL
        );


    if (
        usb_task_result != pdPASS
    ) {

        ESP_LOGE(
            TAG,
            "Cannot create USB task"
        );

        abort();
    }


    /*
     * USB application event processor.
     */

    BaseType_t app_usb_result =

        xTaskCreate(
            usb_application_task,
            "usb_app",
            8192,
            NULL,
            5,
            NULL
        );


    if (
        app_usb_result != pdPASS
    ) {

        ESP_LOGE(
            TAG,
            "Cannot create USB app task"
        );

        abort();
    }


    /*
     * Audio scanner/uploader.
     */

    BaseType_t audio_result =

        xTaskCreate(
            audio_task,
            "audio_task",
            12288,
            NULL,
            4,
            NULL
        );


    if (
        audio_result != pdPASS
    ) {

        ESP_LOGE(
            TAG,
            "Cannot create audio task"
        );

        abort();
    }


    ESP_LOGI(
        TAG,
        "System initialization complete"
    );


    ESP_LOGI(
        TAG,
        "Waiting for USB flash drive..."
    );
}
