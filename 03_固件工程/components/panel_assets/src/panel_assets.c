#include "panel_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_files.h"
#include "psa/crypto.h"

#define ASSETS_ROOT_REL "/panel/assets"
#define ASSETS_MANIFEST_REL "/panel/assets/manifest.json"
#define ASSETS_WORKER_STACK 12288U
#define ASSETS_PATH_BYTES 512

typedef struct {
    void *raw;
    uint32_t raw_size;
    lv_font_fmt_txt_glyph_dsc_t *glyphs;
    lv_font_fmt_txt_cmap_t *cmaps;
    lv_font_fmt_txt_dsc_t *dsc;
    lv_font_t *font;
} asset_font_owner_t;

typedef struct {
    char path[PANEL_ASSETS_MAX_PATH_BYTES + 1];
    size_t bytes;
    char sha256[65];
    unsigned glyphs;
} asset_spec_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_started;
static bool s_queued;
static bool s_cancel;
static panel_assets_state_t s_state;
static char s_detail[96];
static asset_font_owner_t *s_owner;
static unsigned s_refs;
static uint32_t s_pack_version;
static size_t s_font_file_bytes;
static size_t s_font_ram_bytes;
static bool s_hash_checked;
static volatile uint32_t s_worker_stack_free;

static void set_state(panel_assets_state_t state, const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    snprintf(s_detail, sizeof(s_detail), "%s", detail != NULL ? detail : "");
    xSemaphoreGive(s_lock);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t get_i16(const uint8_t *p) { return (int16_t)get_u16(p); }

static bool path_safe_relative(const char *path)
{
    if (path == NULL) return false;
    size_t n = strlen(path);
    if (n == 0 || n > PANEL_ASSETS_MAX_PATH_BYTES) return false;
    if (path[0] == '/' || path[n - 1] == '/') return false;
    if (strstr(path, "..") != NULL) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') return false;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool read_file_limited(const char *abs_path, size_t cap, void **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    FILE *file = fopen(abs_path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end < 0 || (unsigned long)end > cap) {
        fclose(file);
        return false;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t size = (size_t)end;
    uint8_t *data = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        fclose(file);
        return false;
    }
    size_t got = fread(data, 1, size, file);
    bool ok = got == size && ferror(file) == 0;
    fclose(file);
    if (!ok) {
        heap_caps_free(data);
        return false;
    }
    data[size] = '\0';
    *out = data;
    *out_len = size;
    return true;
}

static bool json_depth_ok(const char *text, size_t len, unsigned max_depth)
{
    unsigned depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return false;
            depth--;
        }
    }
    return !in_string && depth == 0;
}

static bool parse_manifest(const uint8_t *data, size_t len, asset_spec_t *spec)
{
    if (!json_depth_ok((const char *)data, len, 16)) return false;
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (root == NULL) return false;
    bool ok = false;
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "pack_version");
    cJSON *min_fw = cJSON_GetObjectItemCaseSensitive(root, "min_firmware");
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring, PANEL_ASSETS_SCHEMA) != 0) goto done;
    if (!cJSON_IsNumber(version) || version->valueint != PANEL_ASSETS_PACK_VERSION) goto done;
    if (!cJSON_IsString(min_fw) || strcmp(min_fw->valuestring, PANEL_ASSETS_MIN_FIRMWARE) != 0) {
        goto done;
    }
    if (!cJSON_IsArray(assets)) goto done;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, assets) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(name) || !cJSON_IsString(type)) continue;
        if (strcmp(name->valuestring, PANEL_ASSETS_FONT_NAME) != 0) continue;
        if (strcmp(type->valuestring, "font") != 0) continue;

        cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
        cJSON *bytes = cJSON_GetObjectItemCaseSensitive(item, "bytes");
        cJSON *sha = cJSON_GetObjectItemCaseSensitive(item, "sha256");
        cJSON *glyphs = cJSON_GetObjectItemCaseSensitive(item, "glyphs");
        cJSON *bpp = cJSON_GetObjectItemCaseSensitive(item, "bpp");
        if (!cJSON_IsString(path) || !cJSON_IsNumber(bytes) || !cJSON_IsString(sha) ||
            !cJSON_IsNumber(glyphs) || !cJSON_IsNumber(bpp)) {
            goto done;
        }
        if (!path_safe_relative(path->valuestring)) goto done;
        if (bytes->valuedouble <= 0 ||
            bytes->valuedouble > (double)PANEL_ASSETS_MAX_FONT_BYTES) goto done;
        if (strlen(sha->valuestring) != 64) goto done;
        if (bpp->valueint != 4) goto done;
        if (glyphs->valueint <= 0 || glyphs->valueint > (int)PANEL_ASSETS_MAX_GLYPHS) goto done;

        snprintf(spec->path, sizeof(spec->path), "%s", path->valuestring);
        spec->bytes = (size_t)bytes->valuedouble;
        snprintf(spec->sha256, sizeof(spec->sha256), "%s", sha->valuestring);
        spec->glyphs = (unsigned)glyphs->valueint;
        s_pack_version = (uint32_t)version->valueint;
        ok = true;
        break;
    }

done:
    cJSON_Delete(root);
    return ok;
}

static bool sha256_file_matches(const uint8_t *data, size_t len, const char *expected)
{
    unsigned char digest[32];
    size_t digest_len = 0;
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, len, digest, sizeof(digest), &digest_len) !=
        PSA_SUCCESS || digest_len != sizeof(digest)) {
        return false;
    }
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
    return strcmp(hex, expected) == 0;
}

static void free_owner_parts(asset_font_owner_t *owner)
{
    if (owner == NULL) return;
    if (owner->glyphs != NULL) heap_caps_free(owner->glyphs);
    if (owner->cmaps != NULL) heap_caps_free(owner->cmaps);
    if (owner->dsc != NULL) heap_caps_free(owner->dsc);
    if (owner->font != NULL) heap_caps_free(owner->font);
    heap_caps_free(owner);
}

static void free_owner(asset_font_owner_t *owner)
{
    if (owner == NULL) return;
    if (owner->raw != NULL) heap_caps_free(owner->raw);
    free_owner_parts(owner);
}

static asset_font_owner_t *build_font(uint8_t *raw, uint32_t raw_size, unsigned expected_glyphs)
{
    if (raw_size < 72 || memcmp(raw, "PFNT", 4) != 0) return NULL;
    if (get_u16(raw + 4) != 1 || get_u16(raw + 6) < 72) return NULL;
    if (get_u32(raw + 8) != raw_size) return NULL;
    uint32_t flags = get_u32(raw + 12);
    if ((flags & 0xFF) != 1 || ((flags >> 8) & 0xFF) != 4) return NULL;
    uint32_t glyph_count = get_u32(raw + 16);
    uint32_t cmap_count = get_u32(raw + 20);
    if (glyph_count < 2 || glyph_count > PANEL_ASSETS_MAX_GLYPHS) return NULL;
    if (cmap_count != 1 || glyph_count != expected_glyphs) return NULL;
    int16_t ascent = get_i16(raw + 24);
    int16_t descent = get_i16(raw + 26);
    int16_t line_height = get_i16(raw + 28);
    int16_t base_line = get_i16(raw + 30);
    if (ascent <= 0 || ascent > 256 || descent > 0 || descent < -256) return NULL;
    if (line_height <= 0 || line_height > 256 || base_line < 0 || base_line > line_height) {
        return NULL;
    }

    uint32_t glyph_off = get_u32(raw + 36);
    uint32_t cmap_off = get_u32(raw + 40);
    uint32_t bitmap_off = get_u32(raw + 44);
    uint32_t bitmap_size = get_u32(raw + 48);
    uint32_t unicode_off = get_u32(raw + 52);
    uint32_t gid_off = get_u32(raw + 56);
    uint32_t unicode_count = get_u32(raw + 60);
    (void)cmap_off;

    if (unicode_count == 0 || unicode_count > glyph_count - 1U) return NULL;
    if ((uint64_t)glyph_off + (uint64_t)glyph_count * 16 > raw_size) return NULL;
    if ((uint64_t)unicode_off + (uint64_t)unicode_count * 2 > raw_size) return NULL;
    if ((uint64_t)gid_off + (uint64_t)unicode_count * 2 > raw_size) return NULL;
    if ((uint64_t)bitmap_off + bitmap_size > raw_size) return NULL;
    if (bitmap_size == 0 || (unicode_off & 1U) != 0 || (gid_off & 1U) != 0) return NULL;

    asset_font_owner_t *owner = heap_caps_malloc(sizeof(*owner), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (owner == NULL) return NULL;
    memset(owner, 0, sizeof(*owner));
    owner->raw = raw;
    owner->raw_size = raw_size;
    owner->glyphs = heap_caps_malloc((size_t)glyph_count * sizeof(*owner->glyphs),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    owner->cmaps = heap_caps_malloc(sizeof(*owner->cmaps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    owner->dsc = heap_caps_malloc(sizeof(*owner->dsc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    owner->font = heap_caps_malloc(sizeof(*owner->font), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (owner->glyphs == NULL || owner->cmaps == NULL || owner->dsc == NULL ||
        owner->font == NULL) {
        free_owner_parts(owner);
        return NULL;
    }
    memset(owner->glyphs, 0, (size_t)glyph_count * sizeof(*owner->glyphs));
    memset(owner->cmaps, 0, sizeof(*owner->cmaps));
    memset(owner->dsc, 0, sizeof(*owner->dsc));
    memset(owner->font, 0, sizeof(*owner->font));

    const uint8_t *dsc = raw + glyph_off;
    for (uint32_t i = 0; i < glyph_count; i++) {
        const uint8_t *g = dsc + (size_t)i * 16;
        lv_font_fmt_txt_glyph_dsc_t *out = &owner->glyphs[i];
        out->bitmap_index = get_u32(g);
        out->adv_w = get_u32(g + 4);
        out->box_w = (uint16_t)get_u16(g + 8);
        out->box_h = (uint16_t)get_u16(g + 10);
        out->ofs_x = get_i16(g + 12);
        out->ofs_y = get_i16(g + 14);
        if (out->box_w > 256 || out->box_h > 256 || out->adv_w > 4096 ||
            out->ofs_x < -256 || out->ofs_x > 256 ||
            out->ofs_y < -256 || out->ofs_y > 256) {
            free_owner_parts(owner);
            return NULL;
        }
        uint64_t pixels = (uint64_t)out->box_w * out->box_h;
        uint64_t bytes = (pixels + 1) / 2;
        if ((uint64_t)out->bitmap_index + bytes > bitmap_size) {
            free_owner_parts(owner);
            return NULL;
        }
    }
    if (owner->glyphs[0].box_w != 0 || owner->glyphs[0].box_h != 0 ||
        owner->glyphs[0].bitmap_index != 0) {
        free_owner_parts(owner);
        return NULL;
    }

    const uint16_t *unicode = (const uint16_t *)(raw + unicode_off);
    const uint16_t *gid_ofs = (const uint16_t *)(raw + gid_off);
    for (uint32_t i = 0; i < unicode_count; i++) {
        if (i > 0 && unicode[i] <= unicode[i - 1]) {
            free_owner_parts(owner);
            return NULL;
        }
        if ((uint32_t)gid_ofs[i] > glyph_count - 2U) {
            free_owner_parts(owner);
            return NULL;
        }
    }
    uint32_t range_start = 0;
    uint32_t range_length = (uint32_t)unicode[unicode_count - 1] + 1U;
    if (range_length == 0 || range_length > 0xFFFFU) {
        free_owner_parts(owner);
        return NULL;
    }

    owner->cmaps[0].range_start = range_start;
    owner->cmaps[0].range_length = (uint16_t)range_length;
    owner->cmaps[0].glyph_id_start = 1;
    owner->cmaps[0].unicode_list = unicode;
    owner->cmaps[0].glyph_id_ofs_list = gid_ofs;
    owner->cmaps[0].list_length = (uint16_t)unicode_count;
    owner->cmaps[0].type = LV_FONT_FMT_TXT_CMAP_SPARSE_FULL;

    owner->dsc->glyph_bitmap = raw + bitmap_off;
    owner->dsc->glyph_dsc = owner->glyphs;
    owner->dsc->cmaps = owner->cmaps;
    owner->dsc->kern_dsc = NULL;
    owner->dsc->kern_scale = 0;
    owner->dsc->cmap_num = 1;
    owner->dsc->bpp = 4;
    owner->dsc->kern_classes = 0;
    owner->dsc->bitmap_format = LV_FONT_FMT_TXT_PLAIN;
    owner->dsc->stride = 0;

    owner->font->get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    owner->font->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    owner->font->line_height = line_height;
    owner->font->base_line = base_line;
    owner->font->subpx = LV_FONT_SUBPX_NONE;
    owner->font->underline_position = (int8_t)get_i16(raw + 32);
    owner->font->underline_thickness = (int8_t)get_i16(raw + 34);
    owner->font->dsc = owner->dsc;
    owner->font->fallback = NULL;
    owner->font->user_data = owner;
    return owner;
}

static bool load_font_asset_locked(void)
{
    if (!panel_files_card_ready()) {
        set_state(PANEL_ASSETS_STATE_NO_CARD, "TF卡不可用，使用内置字体");
        return false;
    }
    const char *root = panel_files_root_path();
    if (root == NULL || root[0] == '\0') {
        set_state(PANEL_ASSETS_STATE_NO_CARD, "TF卡不可用，使用内置字体");
        return false;
    }

    char manifest_path[ASSETS_PATH_BYTES];
    int n = snprintf(manifest_path, sizeof(manifest_path), "%s%s", root, ASSETS_MANIFEST_REL);
    if (n < 0 || (size_t)n >= sizeof(manifest_path)) {
        set_state(PANEL_ASSETS_STATE_INVALID, "素材路径过长");
        return false;
    }

    void *manifest = NULL;
    size_t manifest_len = 0;
    if (!read_file_limited(manifest_path, PANEL_ASSETS_MAX_MANIFEST_BYTES,
                           &manifest, &manifest_len)) {
        set_state(PANEL_ASSETS_STATE_MISSING, "素材包清单不存在或读取失败");
        return false;
    }
    asset_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    bool parsed = parse_manifest((const uint8_t *)manifest, manifest_len, &spec);
    heap_caps_free(manifest);
    if (!parsed) {
        set_state(PANEL_ASSETS_STATE_INVALID, "素材包版本或清单不匹配");
        return false;
    }

    char font_path[ASSETS_PATH_BYTES];
    n = snprintf(font_path, sizeof(font_path), "%s%s/%s", root, ASSETS_ROOT_REL, spec.path);
    if (n < 0 || (size_t)n >= sizeof(font_path)) {
        set_state(PANEL_ASSETS_STATE_INVALID, "字体路径过长");
        return false;
    }

    void *raw = NULL;
    size_t raw_len = 0;
    if (!read_file_limited(font_path, PANEL_ASSETS_MAX_FONT_BYTES, &raw, &raw_len)) {
        set_state(PANEL_ASSETS_STATE_MISSING, "字体文件不存在或读取失败");
        return false;
    }
    if (raw_len != spec.bytes) {
        heap_caps_free(raw);
        set_state(PANEL_ASSETS_STATE_CORRUPT, "字体大小与清单不一致");
        return false;
    }
    if (!sha256_file_matches((const uint8_t *)raw, raw_len, spec.sha256)) {
        heap_caps_free(raw);
        set_state(PANEL_ASSETS_STATE_CORRUPT, "字体SHA-256校验失败");
        return false;
    }

    asset_font_owner_t *owner = build_font((uint8_t *)raw, (uint32_t)raw_len, spec.glyphs);
    if (owner == NULL) {
        heap_caps_free(raw);
        set_state(PANEL_ASSETS_STATE_CORRUPT, "字体格式无效或内存不足");
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != PANEL_ASSETS_STATE_LOADING || s_cancel) {
        s_cancel = false;
        if (s_state == PANEL_ASSETS_STATE_LOADING) {
            s_state = PANEL_ASSETS_STATE_IDLE;
            snprintf(s_detail, sizeof(s_detail), "%s", "已取消外置字体加载");
        }
        xSemaphoreGive(s_lock);
        free_owner(owner);
        return false;
    }
    s_owner = owner;
    s_font_file_bytes = raw_len;
    s_font_ram_bytes = raw_len + (size_t)spec.glyphs * sizeof(lv_font_fmt_txt_glyph_dsc_t) +
                       sizeof(lv_font_fmt_txt_cmap_t) + sizeof(lv_font_fmt_txt_dsc_t) +
                       sizeof(lv_font_t) + sizeof(asset_font_owner_t);
    s_hash_checked = true;
    s_state = PANEL_ASSETS_STATE_READY;
    snprintf(s_detail, sizeof(s_detail), "%s", "外置中文字体已加载");
    xSemaphoreGive(s_lock);
    return true;
}

static bool load_font_asset(void)
{
    if (!panel_files_io_acquire()) {
        set_state(PANEL_ASSETS_STATE_IO_ERROR, "TF卡I/O忙，稍后重试");
        return false;
    }
    bool ok = load_font_asset_locked();
    panel_files_io_release();
    return ok;
}

static void update_worker_stack_free(void)
{
    s_worker_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

static void assets_worker(void *arg)
{
    (void)arg;
    update_worker_stack_free();
    uint8_t command;
    for (;;) {
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_queued = false;
        if (s_state == PANEL_ASSETS_STATE_READY) {
            xSemaphoreGive(s_lock);
            continue;
        }
        s_state = PANEL_ASSETS_STATE_LOADING;
        snprintf(s_detail, sizeof(s_detail), "%s", "正在读取外置字体");
        xSemaphoreGive(s_lock);
        (void)load_font_asset();
        update_worker_stack_free();
    }
}

esp_err_t panel_assets_init(void)
{
    if (s_started) return ESP_OK;
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if (s_queue == NULL) s_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_lock == NULL || s_queue == NULL) return ESP_ERR_NO_MEM;
    s_state = PANEL_ASSETS_STATE_IDLE;
    s_detail[0] = '\0';
    if (xTaskCreate(assets_worker, "panel_assets", ASSETS_WORKER_STACK, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t panel_assets_request_font(void)
{
    if (!s_started || s_lock == NULL || s_queue == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == PANEL_ASSETS_STATE_READY || s_queued) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_state == PANEL_ASSETS_STATE_LOADING) {
        s_cancel = false;
        snprintf(s_detail, sizeof(s_detail), "%s", "正在读取外置字体");
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    uint8_t command = 1;
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }
    s_queued = true;
    s_state = PANEL_ASSETS_STATE_LOADING;
    s_cancel = false;
    s_hash_checked = false;
    s_font_file_bytes = 0;
    s_font_ram_bytes = 0;
    snprintf(s_detail, sizeof(s_detail), "%s", "等待加载外置字体");
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const lv_font_t *panel_assets_acquire_font(void)
{
    if (s_lock == NULL) return NULL;
    const lv_font_t *font = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == PANEL_ASSETS_STATE_READY && s_owner != NULL) {
        s_refs++;
        font = s_owner->font;
    }
    xSemaphoreGive(s_lock);
    return font;
}

void panel_assets_release_font(void)
{
    if (s_lock == NULL) return;
    asset_font_owner_t *drop = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refs > 0) s_refs--;
    if (s_refs == 0 && s_owner != NULL) {
        drop = s_owner;
        s_owner = NULL;
        s_state = PANEL_ASSETS_STATE_IDLE;
        snprintf(s_detail, sizeof(s_detail), "%s", "外置字体已释放");
        s_font_ram_bytes = 0;
    } else if (s_refs == 0 && s_state == PANEL_ASSETS_STATE_LOADING) {
        s_cancel = true;
    }
    xSemaphoreGive(s_lock);
    free_owner(drop);
}

void panel_assets_cancel_font(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == PANEL_ASSETS_STATE_LOADING) {
        s_cancel = true;
        snprintf(s_detail, sizeof(s_detail), "%s", "正在取消外置字体加载");
    } else if (s_state == PANEL_ASSETS_STATE_READY && s_refs == 0 && s_owner != NULL) {
        asset_font_owner_t *drop = s_owner;
        s_owner = NULL;
        s_state = PANEL_ASSETS_STATE_IDLE;
        snprintf(s_detail, sizeof(s_detail), "%s", "外置字体已释放");
        s_font_ram_bytes = 0;
        xSemaphoreGive(s_lock);
        free_owner(drop);
        return;
    }
    xSemaphoreGive(s_lock);
}

void panel_assets_get_status(panel_assets_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        out->state = PANEL_ASSETS_STATE_IDLE;
        snprintf(out->detail, sizeof(out->detail), "%s", "未初始化");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_state;
    snprintf(out->detail, sizeof(out->detail), "%s", s_detail);
    out->pack_version = s_pack_version;
    snprintf(out->font_name, sizeof(out->font_name), "%s", PANEL_ASSETS_FONT_NAME);
    out->font_file_bytes = s_font_file_bytes;
    out->font_ram_bytes = s_font_ram_bytes;
    out->hash_checked = s_hash_checked;
    out->refs = s_refs;
    xSemaphoreGive(s_lock);
}

uint32_t panel_assets_worker_stack_free(void)
{
    return s_worker_stack_free;
}
