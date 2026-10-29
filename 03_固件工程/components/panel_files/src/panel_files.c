#include "panel_files.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_files_platform.h"

#define FILES_QUEUE_LEN 8U
#define FILES_WORKER_STACK 12288U
#define FILES_SCAN_TIMEOUT_MS 1500U
#define FILES_ABS_PATH_BYTES (PANEL_FILES_MAX_PATH + 384)

typedef enum {
    FILES_CMD_RETRY = 1,
    FILES_CMD_LIST,
    FILES_CMD_PREVIEW,
    FILES_CMD_CLOSE,
} files_cmd_type_t;

typedef struct {
    files_cmd_type_t type;
    uint32_t request_id;
    unsigned index;
    unsigned page;
    char path[PANEL_FILES_MAX_PATH];
    char preview_path[FILES_ABS_PATH_BYTES];
    char preview_name[PANEL_FILES_NAME_BYTES];
    uint64_t preview_size;
    uint32_t preview_mtime;
    bool preview_valid_utf8;
    bool preview_valid_name;
} files_cmd_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_io_lock;
static uint32_t s_pin_count;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_started;
static uint32_t s_request_id;
static uint32_t s_revision;
static panel_files_snapshot_t *s_work;
static panel_files_snapshot_t *s_published;
static char *s_preview_io;
static volatile uint32_t s_worker_stack_free;

/* ------------------------------------------------------------------------- */
/* UTF-8                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Validate a complete UTF-8 buffer.  A valid but short tail is reported via
 * incomplete_bytes, never silently accepted.  Existing continuation bytes are
 * always checked, including E4 41 (invalid) versus E4 B8 (valid short tail).
 */
static bool utf8_scan(const char *text, size_t len, size_t *incomplete_bytes)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;
    if (incomplete_bytes != NULL) *incomplete_bytes = 0;
    while (i < len) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }

        int extra;
        unsigned char second_lo = 0x80;
        unsigned char second_hi = 0xBF;
        unsigned min_cp;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            min_cp = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            min_cp = 0x800;
            if (c == 0xE0) second_lo = 0xA0;
            else if (c == 0xED) second_hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            min_cp = 0x10000;
            if (c == 0xF0) second_lo = 0x90;
            else if (c == 0xF4) second_hi = 0x8F;
        } else {
            return false;
        }

        if (i + (size_t)extra >= len) {
            size_t available = len - i - 1;
            for (size_t j = 0; j < available; j++) {
                unsigned char cc = s[i + 1 + j];
                if ((cc & 0xC0) != 0x80) return false;
                if (j == 0 && (cc < second_lo || cc > second_hi)) return false;
            }
            if (incomplete_bytes != NULL) *incomplete_bytes = len - i;
            return true;
        }

        if (s[i + 1] < second_lo || s[i + 1] > second_hi) return false;
        unsigned cp = (extra == 1) ? (c & 0x1F) : (extra == 2) ? (c & 0x0F) : (c & 0x07);
        for (int j = 1; j <= extra; j++) {
            unsigned char cc = s[i + (size_t)j];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t)extra + 1;
    }
    return true;
}

static size_t utf8_char_bytes(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 0;
}

/* Longest valid, complete UTF-8 prefix; used only for clipped long names. */
static size_t utf8_complete_prefix(const char *text, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t n = utf8_char_bytes((unsigned char)text[i]);
        if (n == 0 || i + n > len) break;
        size_t incomplete = 0;
        if (!utf8_scan(text + i, n, &incomplete) || incomplete != 0) break;
        i += n;
    }
    return i;
}

static bool component_valid(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > 127) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}

static bool path_valid(const char *path)
{
    if (path == NULL || path[0] != '/') return false;
    if (strcmp(path, "/") == 0) return true;
    size_t len = strlen(path);
    if (len == 0 || len >= PANEL_FILES_MAX_PATH) return false;
    size_t incomplete = 0;
    if (!utf8_scan(path, len, &incomplete) || incomplete != 0) return false;

    unsigned depth = 0;
    const char *p = path;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') return false;
        if (c == '/') {
            if (p[1] == '\0' || p[1] == '/') return false;
            depth++;
            if (depth > PANEL_FILES_MAX_DEPTH) return false;
        }
        p++;
    }

    const char *scan = path;
    while (*scan) {
        const char *start = scan + 1;
        const char *end = strchr(start, '/');
        size_t n = end != NULL ? (size_t)(end - start) : strlen(start);
        if (n == 0) return false;
        if ((n == 1 && start[0] == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.')) return false;
        if (end == NULL) break;
        scan = end;
    }
    return true;
}

static bool join_relative(const char *dir, const char *name, char *out, size_t cap)
{
    if (dir == NULL || name == NULL || out == NULL || cap == 0) return false;
    if (!path_valid(dir) || !component_valid(name)) return false;
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    if (strcmp(dir, "/") == 0) {
        if (dl + nl + 1 > cap) return false;
        snprintf(out, cap, "/%s", name);
    } else {
        if (dl + nl + 2 > cap) return false;
        memcpy(out, dir, dl);
        out[dl] = '/';
        memcpy(out + dl + 1, name, nl);
        out[dl + 1 + nl] = '\0';
    }
    return path_valid(out);
}

static bool mount_absolute(const char *relative, char *out, size_t cap)
{
    if (relative == NULL || out == NULL || cap == 0) return false;
    if (!path_valid(relative)) return false;
    const char *root = panel_files_platform_root();
    if (root == NULL || root[0] == '\0') return false;
    size_t rl = strlen(root);
    size_t pl = strlen(relative);
    if (strcmp(relative, "/") == 0) {
        if (rl + 1 > cap) return false;
        memcpy(out, root, rl + 1);
        return true;
    }
    size_t need = rl + pl + 1;
    if (root[rl - 1] == '/') need = rl + pl;
    if (need > cap) return false;
    memcpy(out, root, rl);
    size_t o = rl;
    if (root[rl - 1] != '/') out[o++] = '/';
    memcpy(out + o, relative + 1, pl);
    out[o + pl - 1] = '\0';
    return true;
}

/* ------------------------------------------------------------------------- */
/* Snapshot publishing                                                       */
/* ------------------------------------------------------------------------- */

static bool request_is_current(uint32_t request_id, bool startup)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = startup ? (s_request_id == 0 && request_id == 0) :
                        (request_id != 0 && request_id == s_request_id);
    xSemaphoreGive(s_lock);
    return ok;
}

static bool publish_work(panel_files_snapshot_t *work, uint32_t request_id, bool startup)
{
    if (s_lock == NULL || work == NULL || s_published == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = startup ? (s_request_id == 0 && request_id == 0) :
                        (request_id != 0 && request_id == s_request_id);
    if (ok) {
        s_revision++;
        if (s_revision == 0) s_revision = 1;
        work->generation = s_revision;
        *s_published = *work;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static void clear_preview(panel_files_snapshot_t *snap)
{
    snap->preview_open = false;
    snap->preview_error = false;
    snap->preview_error_text[0] = '\0';
    snap->preview_name[0] = '\0';
    snap->preview_size = 0;
    snap->preview_truncated = false;
    snap->preview_type[0] = '\0';
    snap->preview_mtime = 0;
    snap->preview_mtime_valid = false;
    snap->preview_mtime_text[0] = '\0';
    snap->preview_len = 0;
    snap->preview[0] = '\0';
}

static void set_preview_error(panel_files_snapshot_t *snap, const char *text)
{
    snap->preview_open = false;
    snap->preview_error = true;
    snap->preview_len = 0;
    snap->preview[0] = '\0';
    if (snap->preview_name[0] == '\0') {
        snprintf(snap->preview_name, sizeof(snap->preview_name), "%s", "文件");
    }
    snprintf(snap->preview_error_text, sizeof(snap->preview_error_text), "%s",
             text != NULL ? text : "无法预览文件");
    snprintf(snap->error, sizeof(snap->error), "%s", text != NULL ? text : "无法预览文件");
}

static void fail_list(panel_files_snapshot_t *snap, panel_files_state_t state,
                      const char *path, const char *text, uint32_t request_id,
                      bool startup)
{
    snap->state = state;
    snap->count = 0;
    snap->total_entries = 0;
    snap->page = 0;
    snap->page_count = 0;
    snap->truncated = false;
    snprintf(snap->path, sizeof(snap->path), "%s", path != NULL ? path : "/");
    snprintf(snap->error, sizeof(snap->error), "%s", text != NULL ? text : "读取文件或目录失败");
    clear_preview(snap);
    publish_work(snap, request_id, startup);
}

static void fill_mount_error(esp_err_t err, panel_files_state_t *state, const char **text)
{
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_TIMEOUT) {
        *state = PANEL_FILES_STATE_NO_CARD;
        *text = "未检测到可用TF卡";
    } else if (err == ESP_ERR_INVALID_STATE) {
        *state = PANEL_FILES_STATE_ERROR;
        *text = "TF卡服务未就绪，请稍后重试";
    } else {
        *state = PANEL_FILES_STATE_ERROR;
        *text = "挂载TF卡失败，请重试";
    }
}

/* ------------------------------------------------------------------------- */
/* Directory scanning                                                        */
/* ------------------------------------------------------------------------- */

static void make_entry(const panel_files_platform_dirent_t *de, panel_files_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->is_dir = de->is_dir;
    entry->size = de->size;
    entry->mtime = de->mtime;

    size_t n = 0;
    while (n < sizeof(de->name) && de->name[n] != '\0') n++;
    bool source_truncated = de->name_truncated || n >= sizeof(de->name);
    size_t incomplete = 0;
    bool source_valid = utf8_scan(de->name, n, &incomplete);

    if (!source_valid) {
        snprintf(entry->name, sizeof(entry->name), "%s", "（文件名编码无效）");
        entry->valid_name = false;
        entry->valid_utf8 = false;
        entry->truncated_name = false;
        return;
    }

    size_t usable = n;
    if (incomplete != 0) {
        usable = utf8_complete_prefix(de->name, n);
        source_truncated = true;
    }

    size_t out = 0;
    size_t i = 0;
    bool clipped = false;
    while (i < usable) {
        size_t clen = utf8_char_bytes((unsigned char)de->name[i]);
        if (clen == 0 || i + clen > usable) {
            source_valid = false;
            break;
        }
        if (out + clen >= sizeof(entry->name)) {
            clipped = true;
            break;
        }
        memcpy(entry->name + out, de->name + i, clen);
        out += clen;
        i += clen;
    }
    entry->name[out] = '\0';

    if (!source_valid || out == 0) {
        snprintf(entry->name, sizeof(entry->name), "%s", "（文件名编码无效）");
        entry->valid_name = false;
        entry->valid_utf8 = false;
        entry->truncated_name = false;
        return;
    }

    entry->valid_utf8 = true;
    entry->truncated_name = source_truncated || clipped;
    entry->valid_name = !entry->truncated_name && component_valid(entry->name);
}

/*
 * One streaming pass.  Only the requested page is stored in the 8 KiB worker
 * snapshot; total/pages are counted without a 73 KiB all[512] stack array.
 */
static bool scan_pass(const char *relative, unsigned page, panel_files_snapshot_t *work,
                      unsigned *total_out, bool *truncated_out)
{
    char full[FILES_ABS_PATH_BYTES];
    if (!mount_absolute(relative, full, sizeof(full))) {
        snprintf(work->error, sizeof(work->error), "%s", "文件路径无效");
        return false;
    }

    panel_files_platform_dir_t *dir = NULL;
    esp_err_t err = panel_files_platform_opendir(full, &dir);
    if (err != ESP_OK || dir == NULL) {
        snprintf(work->error, sizeof(work->error), "%s", "读取文件或目录失败");
        return false;
    }

    unsigned total = 0;
    bool truncated = false;
    uint32_t start = xTaskGetTickCount();
    work->count = 0;
    const uint64_t page_start = (uint64_t)page * PANEL_FILES_MAX_ENTRIES;

    for (;;) {
        panel_files_platform_dirent_t de;
        memset(&de, 0, sizeof(de));
        err = panel_files_platform_readdir(dir, &de);
        if (err == ESP_ERR_NOT_FOUND) break;
        if (err != ESP_OK) {
            panel_files_platform_closedir(dir);
            snprintf(work->error, sizeof(work->error), "%s", "读取文件或目录失败");
            return false;
        }
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
        if (total >= PANEL_FILES_SCAN_LIMIT) {
            truncated = true;
            break;
        }

        unsigned index = total++;
        if (index >= page_start && index < page_start + PANEL_FILES_MAX_ENTRIES &&
            work->count < PANEL_FILES_MAX_ENTRIES) {
            make_entry(&de, &work->entries[work->count++]);
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(FILES_SCAN_TIMEOUT_MS)) {
            truncated = true;
            break;
        }
    }
    panel_files_platform_closedir(dir);
    if (total >= PANEL_FILES_SCAN_LIMIT) truncated = true;
    *total_out = total;
    *truncated_out = truncated;
    return true;
}

static void list_dir(const char *path, unsigned page, uint32_t request_id, bool startup)
{
    if (!request_is_current(request_id, startup)) return;
    panel_files_snapshot_t *work = s_work;
    memset(work, 0, sizeof(*work));
    work->state = PANEL_FILES_STATE_READY;
    snprintf(work->path, sizeof(work->path), "%s", path != NULL ? path : "/");

    if (!path_valid(work->path)) {
        fail_list(work, PANEL_FILES_STATE_ERROR, "/", "文件路径无效", request_id, startup);
        return;
    }

    unsigned total = 0;
    bool truncated = false;
    char error_copy[sizeof(work->error)];
    if (!scan_pass(work->path, page, work, &total, &truncated)) {
        memcpy(error_copy, work->error, sizeof(error_copy));
        error_copy[sizeof(error_copy) - 1] = '\0';
        fail_list(work, PANEL_FILES_STATE_ERROR, work->path, error_copy, request_id, startup);
        return;
    }
    unsigned pages = total == 0 ? 1U :
                     (total + PANEL_FILES_MAX_ENTRIES - 1U) / PANEL_FILES_MAX_ENTRIES;
    if (page >= pages) {
        page = pages - 1U;
        if (!scan_pass(work->path, page, work, &total, &truncated)) {
            memcpy(error_copy, work->error, sizeof(error_copy));
            error_copy[sizeof(error_copy) - 1] = '\0';
            fail_list(work, PANEL_FILES_STATE_ERROR, work->path, error_copy, request_id, startup);
            return;
        }
        pages = total == 0 ? 1U :
                (total + PANEL_FILES_MAX_ENTRIES - 1U) / PANEL_FILES_MAX_ENTRIES;
        if (page >= pages) page = pages - 1U;
    }

    work->state = PANEL_FILES_STATE_READY;
    work->page = page;
    work->page_count = pages;
    work->total_entries = total;
    work->truncated = truncated;
    work->error[0] = '\0';
    clear_preview(work);
    publish_work(work, request_id, startup);
}

/* ------------------------------------------------------------------------- */
/* Preview                                                                   */
/* ------------------------------------------------------------------------- */

static bool preview_extension_ok(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;
    return strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".md") == 0 ||
           strcasecmp(dot, ".log") == 0 || strcasecmp(dot, ".csv") == 0 ||
           strcasecmp(dot, ".json") == 0;
}

static void preview_fail(panel_files_snapshot_t *work, uint32_t request_id, const char *text)
{
    set_preview_error(work, text);
    publish_work(work, request_id, false);
}

static void set_preview_meta(panel_files_snapshot_t *work, uint32_t mtime)
{
    work->preview_mtime = mtime;
    work->preview_mtime_valid = mtime != 0;
    work->preview_mtime_text[0] = '\0';
    if (mtime != 0) {
        time_t t = (time_t)mtime;
        struct tm local;
        bool have = false;
#ifdef _WIN32
        struct tm *tmp = localtime(&t);
        if (tmp != NULL) {
            local = *tmp;
            have = true;
        }
#else
        if (localtime_r(&t, &local) != NULL) have = true;
#endif
        if (have) {
            strftime(work->preview_mtime_text, sizeof(work->preview_mtime_text),
                     "%Y-%m-%d %H:%M", &local);
        }
    }
}

static void open_preview(const files_cmd_t *cmd)
{
    if (!request_is_current(cmd->request_id, false)) return;
    panel_files_snapshot_t *work = s_work;
    work->preview_open = false;
    work->preview_error = false;
    work->preview_error_text[0] = '\0';
    work->preview_len = 0;
    work->preview[0] = '\0';
    work->preview_size = cmd->preview_size;
    work->preview_truncated = false;
    snprintf(work->preview_name, sizeof(work->preview_name), "%s", cmd->preview_name);
    snprintf(work->preview_type, sizeof(work->preview_type), "%s", "未知");
    set_preview_meta(work, cmd->preview_mtime);

    if (!cmd->preview_valid_name || !cmd->preview_valid_utf8) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "文件名编码无效");
        preview_fail(work, cmd->request_id, "文件名编码无效，无法预览");
        return;
    }

    bool is_dir = false;
    uint64_t size = 0;
    uint32_t mtime = 0;
    if (panel_files_platform_stat(cmd->preview_path, &is_dir, &size, &mtime) != ESP_OK) {
        preview_fail(work, cmd->request_id, "文件已不存在或无法读取");
        return;
    }
    work->preview_size = size;
    set_preview_meta(work, mtime);
    if (is_dir) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "文件夹");
        preview_fail(work, cmd->request_id, "文件夹不能预览");
        return;
    }
    if (!preview_extension_ok(cmd->preview_name)) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "其他文件");
        preview_fail(work, cmd->request_id, "该文件类型不支持预览");
        return;
    }

    size_t len = 0;
    bool truncated = false;
    esp_err_t err = panel_files_platform_read(cmd->preview_path, s_preview_io,
                                              PANEL_FILES_PREVIEW_BYTES, &len, &truncated);
    if (err != ESP_OK) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "读取失败");
        preview_fail(work, cmd->request_id, "读取文件失败");
        return;
    }
    if (len > PANEL_FILES_PREVIEW_BYTES) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "读取失败");
        preview_fail(work, cmd->request_id, "读取文件失败");
        return;
    }
    if (memchr(s_preview_io, '\0', len) != NULL) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "二进制/非文本");
        preview_fail(work, cmd->request_id, "文件包含二进制数据，无法预览");
        return;
    }

    size_t incomplete = 0;
    if (!utf8_scan(s_preview_io, len, &incomplete)) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "非 UTF-8 文本");
        preview_fail(work, cmd->request_id, "文件不是有效的UTF-8文本");
        return;
    }
    if (!truncated && incomplete != 0) {
        snprintf(work->preview_type, sizeof(work->preview_type), "%s", "非 UTF-8 文本");
        preview_fail(work, cmd->request_id, "文件不是有效的UTF-8文本");
        return;
    }
    size_t use = len;
    if (incomplete != 0) use -= incomplete;

    size_t start = 0;
    if (use >= 3 && (unsigned char)s_preview_io[0] == 0xEF &&
        (unsigned char)s_preview_io[1] == 0xBB && (unsigned char)s_preview_io[2] == 0xBF) {
        start = 3;
    }
    size_t out = 0;
    for (size_t i = start; i < use; i++) {
        if (s_preview_io[i] == '\r') {
            if (i + 1 < use && s_preview_io[i + 1] == '\n') i++;
            work->preview[out++] = '\n';
        } else {
            work->preview[out++] = s_preview_io[i];
        }
    }
    work->preview[out] = '\0';

    work->preview_len = out;
    work->preview_truncated = truncated || incomplete != 0;
    work->preview_open = true;
    work->preview_error = false;
    work->preview_error_text[0] = '\0';
    snprintf(work->preview_type, sizeof(work->preview_type), "%s", "UTF-8 文本");
    work->preview_size = size;
    snprintf(work->preview_name, sizeof(work->preview_name), "%s", cmd->preview_name);
    work->error[0] = '\0';
    publish_work(work, cmd->request_id, false);
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

static void do_retry(uint32_t request_id)
{
    if (!request_is_current(request_id, false)) return;
    panel_files_snapshot_t *work = s_work;
    memset(work, 0, sizeof(*work));
    work->state = PANEL_FILES_STATE_MOUNTING;
    snprintf(work->path, sizeof(work->path), "%s", "/");
    clear_preview(work);
    publish_work(work, request_id, false);

    if (!panel_files_io_acquire()) {
        fail_list(work, PANEL_FILES_STATE_ERROR, "/", "TF卡忙，请稍后重试",
                  request_id, false);
        return;
    }
    if (!request_is_current(request_id, false)) {
        panel_files_io_release();
        return;
    }
    if (s_pin_count > 0) {
        panel_files_io_release();
        fail_list(work, PANEL_FILES_STATE_ERROR, "/", "TF卡正在播放/读取，稍后重试",
                  request_id, false);
        return;
    }
    panel_files_platform_unmount();
    esp_err_t err = panel_files_platform_mount();
    panel_files_io_release();
    if (err != ESP_OK) {        panel_files_state_t state;
        const char *text;
        fill_mount_error(err, &state, &text);
        fail_list(work, state, "/", text, request_id, false);
        return;
    }
    list_dir("/", 0, request_id, false);
}

static void handle_command(const files_cmd_t *cmd)
{
    if (cmd == NULL) return;
    if (!request_is_current(cmd->request_id, false)) return;
    panel_files_snapshot_t *work = s_work;

    if (cmd->type == FILES_CMD_RETRY) {
        do_retry(cmd->request_id);
        return;
    }
    if (cmd->type == FILES_CMD_CLOSE) {
        if (!request_is_current(cmd->request_id, false)) return;
        clear_preview(work);
        if (work->error[0] != '\0') work->error[0] = '\0';
        publish_work(work, cmd->request_id, false);
        return;
    }
    if (!panel_files_platform_mounted()) {
        fail_list(work, PANEL_FILES_STATE_ERROR, work->path,
                  "TF卡未挂载，请重试", cmd->request_id, false);
        return;
    }

    switch (cmd->type) {
    case FILES_CMD_LIST:
        list_dir(cmd->path, cmd->page, cmd->request_id, false);
        break;
    case FILES_CMD_PREVIEW:
        open_preview(cmd);
        break;
    default:
        break;
    }
}

static void update_worker_stack_free(void)
{
    s_worker_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

static void files_worker(void *arg)
{
    (void)arg;
    update_worker_stack_free();
    panel_files_snapshot_t *work = s_work;
    memset(work, 0, sizeof(*work));
    work->state = PANEL_FILES_STATE_MOUNTING;
    snprintf(work->path, sizeof(work->path), "%s", "/");
    publish_work(work, 0, true);

    esp_err_t err = ESP_FAIL;
    if (panel_files_io_acquire()) {
        err = panel_files_platform_mount();
        panel_files_io_release();
    }
    if (err != ESP_OK) {
        panel_files_state_t state;
        const char *text;
        fill_mount_error(err, &state, &text);
        memset(work, 0, sizeof(*work));
        fail_list(work, state, "/", text, 0, true);
    } else {
        list_dir("/", 0, 0, true);
    }
    update_worker_stack_free();

    files_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            handle_command(&cmd);
            update_worker_stack_free();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

static esp_err_t submit_request(files_cmd_t *cmd, uint32_t expected_generation,
                                bool allow_startup_generation)
{
    if (!s_started || s_queue == NULL || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (cmd == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t result = ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_published == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_generation != s_published->generation &&
        !(allow_startup_generation && expected_generation == 0)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t new_id = s_request_id + 1;
    if (new_id == 0) new_id = 1;
    cmd->request_id = new_id;
    if (xQueueSend(s_queue, cmd, 0) == pdTRUE) {
        s_request_id = new_id;
        result = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t panel_files_init(void)
{
    if (s_started) return ESP_OK;
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if (s_io_lock == NULL) s_io_lock = xSemaphoreCreateMutex();
    if (s_queue == NULL) s_queue = xQueueCreate(FILES_QUEUE_LEN, sizeof(files_cmd_t));
    if (s_work == NULL) {
        s_work = heap_caps_malloc(sizeof(*s_work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_published == NULL) {
        s_published = heap_caps_malloc(sizeof(*s_published), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_preview_io == NULL) {
        s_preview_io = heap_caps_malloc(PANEL_FILES_PREVIEW_BYTES + 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_lock == NULL || s_io_lock == NULL || s_queue == NULL || s_work == NULL ||
        s_published == NULL || s_preview_io == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_work, 0, sizeof(*s_work));
    memset(s_published, 0, sizeof(*s_published));
    s_work->state = PANEL_FILES_STATE_INIT;
    s_published->state = PANEL_FILES_STATE_INIT;
    s_revision = 0;

    if (xTaskCreate(files_worker, "panel_files", FILES_WORKER_STACK, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void panel_files_get_snapshot(panel_files_snapshot_t *out)
{
    if (out == NULL) return;
    if (s_lock == NULL || s_published == NULL) {
        memset(out, 0, sizeof(*out));
        out->state = PANEL_FILES_STATE_INIT;
        snprintf(out->path, sizeof(out->path), "%s", "/");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = *s_published;
    xSemaphoreGive(s_lock);
}

bool panel_files_card_ready(void)
{
    if (s_lock == NULL || s_published == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_published->state == PANEL_FILES_STATE_READY;
    xSemaphoreGive(s_lock);
    return ready && panel_files_platform_mounted();
}

const char *panel_files_root_path(void)
{
    return panel_files_platform_root();
}

bool panel_files_io_acquire(void)
{
    if (s_io_lock == NULL) return false;
    return xSemaphoreTake(s_io_lock, portMAX_DELAY) == pdTRUE;
}

void panel_files_io_release(void)
{
    if (s_io_lock != NULL) xSemaphoreGive(s_io_lock);
}

bool panel_files_mount_pin_acquire(void)
{
    if (s_io_lock == NULL) return false;
    if (xSemaphoreTake(s_io_lock, portMAX_DELAY) != pdTRUE) return false;
    bool ok = panel_files_card_ready() && s_pin_count < 0xFFFFFFFFU;
    if (ok) s_pin_count++;
    xSemaphoreGive(s_io_lock);
    return ok;
}

void panel_files_mount_pin_release(void)
{
    if (s_io_lock == NULL) return;
    if (xSemaphoreTake(s_io_lock, portMAX_DELAY) == pdTRUE) {
        if (s_pin_count > 0) s_pin_count--;
        xSemaphoreGive(s_io_lock);
    }
}

bool panel_files_mount_pin_held(void)
{
    if (s_io_lock == NULL) return false;
    if (xSemaphoreTake(s_io_lock, portMAX_DELAY) != pdTRUE) return false;
    bool held = s_pin_count > 0;
    xSemaphoreGive(s_io_lock);
    return held;
}

uint32_t panel_files_worker_stack_free(void)
{
    return s_worker_stack_free;
}

esp_err_t panel_files_retry(void)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_RETRY;
    /* Retry is explicitly generation-free: it must work from NO_CARD/ERROR and
     * while a stale request is still pending. */
    return submit_request(&cmd, 0, true);
}

esp_err_t panel_files_refresh(uint32_t generation)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_LIST;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_published == NULL || generation != s_published->generation ||
        s_published->state != PANEL_FILES_STATE_READY) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    cmd.page = s_published->page;
    snprintf(cmd.path, sizeof(cmd.path), "%s", s_published->path);
    xSemaphoreGive(s_lock);
    return submit_request(&cmd, generation, false);
}

esp_err_t panel_files_enter(unsigned index, uint32_t generation)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_published == NULL || generation != s_published->generation ||
        s_published->state != PANEL_FILES_STATE_READY || index >= s_published->count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    panel_files_entry_t entry = s_published->entries[index];
    if (!entry.is_dir || !entry.valid_name || !entry.valid_utf8 ||
        !join_relative(s_published->path, entry.name, cmd.path, sizeof(cmd.path))) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreGive(s_lock);
    cmd.type = FILES_CMD_LIST;
    return submit_request(&cmd, generation, false);
}

esp_err_t panel_files_up(uint32_t generation)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_LIST;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_published == NULL || generation != s_published->generation ||
        s_published->state != PANEL_FILES_STATE_READY) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(cmd.path, sizeof(cmd.path), "%s", s_published->path);
    xSemaphoreGive(s_lock);
    if (strcmp(cmd.path, "/") == 0) return ESP_OK;
    char *slash = strrchr(cmd.path, '/');
    if (slash == NULL || slash == cmd.path) {
        snprintf(cmd.path, sizeof(cmd.path), "%s", "/");
    } else {
        *slash = '\0';
    }
    if (!path_valid(cmd.path)) return ESP_ERR_INVALID_ARG;
    return submit_request(&cmd, generation, false);
}

esp_err_t panel_files_root(uint32_t generation)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_LIST;
    snprintf(cmd.path, sizeof(cmd.path), "%s", "/");
    return submit_request(&cmd, generation, true);
}

esp_err_t panel_files_page(int delta, uint32_t generation)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_LIST;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_published == NULL || generation != s_published->generation ||
        s_published->state != PANEL_FILES_STATE_READY) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int next = (int)s_published->page + delta;
    if (next < 0) next = 0;
    if (s_published->page_count == 0) next = 0;
    else if (next >= (int)s_published->page_count) next = (int)s_published->page_count - 1;
    cmd.page = (unsigned)next;
    snprintf(cmd.path, sizeof(cmd.path), "%s", s_published->path);
    xSemaphoreGive(s_lock);
    return submit_request(&cmd, generation, false);
}

esp_err_t panel_files_preview(unsigned index, uint32_t generation)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_PREVIEW;
    cmd.index = index;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_published == NULL || generation != s_published->generation ||
        s_published->state != PANEL_FILES_STATE_READY || index >= s_published->count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    panel_files_entry_t entry = s_published->entries[index];
    char relative[PANEL_FILES_MAX_PATH];
    if (entry.is_dir || !entry.valid_name || !entry.valid_utf8 ||
        !join_relative(s_published->path, entry.name, relative, sizeof(relative))) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (!mount_absolute(relative, cmd.preview_path, sizeof(cmd.preview_path))) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(cmd.preview_name, sizeof(cmd.preview_name), "%s", entry.name);
    cmd.preview_size = entry.size;
    cmd.preview_mtime = entry.mtime;
    cmd.preview_valid_utf8 = entry.valid_utf8;
    cmd.preview_valid_name = entry.valid_name;
    xSemaphoreGive(s_lock);
    return submit_request(&cmd, generation, false);
}

esp_err_t panel_files_close_preview(void)
{
    files_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = FILES_CMD_CLOSE;
    return submit_request(&cmd, 0, true);
}
