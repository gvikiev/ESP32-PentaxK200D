#include "pentax_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "diskio_impl.h"
#include "diskio.h"
#include "ff.h"
#include "esp_vfs_fat.h"

#include "pentax_service.h"
#include "bot.h"

#define SD_BASE_PATH "/sdcard"
#define SD_MAX_FILES 4
#define SD_MAX_SECTORS_PER_IO 32u /* keep each BOT transfer small and predictable */

static const char *TAG = "pentax_sd";

static SemaphoreHandle_t s_mutex;
static bool s_registered; /* diskio + VFS path registered */
static bool s_mounted;    /* f_mount succeeded */
static bool s_probed;     /* READ CAPACITY(10) succeeded; card is ready for disk_read() */
static bool s_want_mount; /* USB link is CONNECTED: mount on first use */
static BYTE s_pdrv = FF_DRV_NOT_USED;
static FATFS *s_fs;
static char s_drv[3];
static uint32_t s_sector_size;
static uint32_t s_sector_count;

/* ------------------------------- diskio driver -------------------------------- *
 * Routed through pentax_service_scsi_io(), which shares the camera's single
 * BOT/mutex, so a read here is naturally serialized against (and pauses
 * for) any Pentax vendor command in flight. */

/* s_probed (set once READ CAPACITY(10) succeeds, before f_mount() is even
 * called) is what these report on, not s_mounted -- f_mount() itself calls
 * disk_initialize()/disk_status() while s_mounted is still false, since
 * that flag only becomes true once f_mount() has returned successfully. */
static DSTATUS ff_sd_status(BYTE pdrv)
{
    (void)pdrv;
    return s_probed ? 0 : STA_NOINIT;
}

static DSTATUS ff_sd_initialize(BYTE pdrv)
{
    (void)pdrv;
    return s_probed ? 0 : STA_NOINIT; /* probing already done by pentax_sd_ensure_mounted() */
}

static DRESULT ff_sd_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    while (count > 0) {
        UINT chunk = count > SD_MAX_SECTORS_PER_IO ? SD_MAX_SECTORS_PER_IO : count;
        uint8_t cdb[10] = { 0 };
        cdb[0] = 0x28; /* READ(10) */
        cdb[2] = (uint8_t)(sector >> 24);
        cdb[3] = (uint8_t)(sector >> 16);
        cdb[4] = (uint8_t)(sector >> 8);
        cdb[5] = (uint8_t)sector;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        size_t want = (size_t)chunk * s_sector_size;
        size_t got = 0;
        esp_err_t err = pentax_service_scsi_io(cdb, sizeof(cdb), BOT_DIR_IN, buff, want, 8000, &got);
        if (err != ESP_OK || got != want) {
            ESP_LOGW(TAG, "READ(10) sector=%u count=%u failed: %s",
                      (unsigned)sector, (unsigned)chunk, esp_err_to_name(err));
            return RES_ERROR;
        }
        buff += want;
        sector += chunk;
        count -= chunk;
    }
    return RES_OK;
}

static DRESULT ff_sd_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    (void)buff;
    (void)sector;
    (void)count;
    return RES_WRPRT; /* mounted read-only: see docs/pentax_k200d_architecture_gemeni.md section 3 */
}

static DRESULT ff_sd_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK; /* nothing buffered on a read-only mount */
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = s_sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)s_sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        return RES_ERROR; /* erase block size: not needed without FF_USE_MKFS */
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t s_diskio_impl = {
    .init = ff_sd_initialize,
    .status = ff_sd_status,
    .read = ff_sd_read,
    .write = ff_sd_write,
    .ioctl = ff_sd_ioctl,
};

/* ------------------------------- lifecycle -------------------------------- */

esp_err_t pentax_sd_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* Call with s_mutex held. */
static void unmount_locked(void)
{
    s_probed = false;
    if (s_mounted) {
        f_mount(NULL, s_drv, 0);
        s_mounted = false;
    }
    if (s_registered) {
        ff_diskio_unregister(s_pdrv);
        esp_vfs_fat_unregister_path(SD_BASE_PATH);
        s_registered = false;
        s_pdrv = FF_DRV_NOT_USED;
    }
}

void pentax_sd_on_link_change(usb_msc_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (state == USB_MSC_CONNECTED) {
        s_want_mount = true;
    } else {
        s_want_mount = false;
        unmount_locked();
    }
    xSemaphoreGive(s_mutex);
}

bool pentax_sd_is_mounted(void)
{
    bool mounted;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    mounted = s_mounted;
    xSemaphoreGive(s_mutex);
    return mounted;
}

const char *pentax_sd_base_path(void)
{
    return SD_BASE_PATH;
}

esp_err_t pentax_sd_ensure_mounted(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mounted) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (!s_want_mount) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE; /* no camera attached */
    }

    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x25; /* READ CAPACITY(10) */
    uint8_t cap[8];
    size_t got = 0;
    esp_err_t err = pentax_service_scsi_io(cdb, sizeof(cdb), BOT_DIR_IN, cap, sizeof(cap), 5000, &got);
    if (err != ESP_OK || got != sizeof(cap)) {
        xSemaphoreGive(s_mutex);
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) | ((uint32_t)cap[2] << 8) | cap[3];
    uint32_t sector_size = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) | ((uint32_t)cap[6] << 8) | cap[7];
    if (sector_size < FF_MIN_SS || sector_size > FF_MAX_SS) {
        ESP_LOGW(TAG, "unsupported SD sector size %u (compiled for %u..%u)",
                  (unsigned)sector_size, (unsigned)FF_MIN_SS, (unsigned)FF_MAX_SS);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_sector_size = sector_size;
    s_sector_count = last_lba + 1;
    s_probed = true;

    if (!s_registered) {
        s_pdrv = FF_DRV_NOT_USED;
        if (ff_diskio_get_drive(&s_pdrv) != ESP_OK || s_pdrv == FF_DRV_NOT_USED) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        ff_diskio_register(s_pdrv, &s_diskio_impl);
        s_drv[0] = (char)('0' + s_pdrv);
        s_drv[1] = ':';
        s_drv[2] = '\0';

        esp_vfs_fat_conf_t conf = {
            .base_path = SD_BASE_PATH,
            .fat_drive = s_drv,
            .max_files = SD_MAX_FILES,
        };
        err = esp_vfs_fat_register_cfg(&conf, &s_fs);
        if (err != ESP_OK) {
            ff_diskio_unregister(s_pdrv);
            s_pdrv = FF_DRV_NOT_USED;
            xSemaphoreGive(s_mutex);
            return err;
        }
        s_registered = true;
    }

    FRESULT fr = f_mount(s_fs, s_drv, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "f_mount failed: %d", (int)fr);
        unmount_locked();
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s (%u x %u-byte sectors)",
              SD_BASE_PATH, (unsigned)s_sector_count, (unsigned)s_sector_size);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
