#include "ui_wallpaper_import.h"
#include "wallimport_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_files.h"

static const char *TAG = "wallimport";

#define WALLPAPER_W 1280U
#define WALLPAPER_H 800U
#define WALLPAPER_BYTES WALLIMPORT_TOTAL_BYTES
#define WALLIMPORT_LINE 2304U
#define WALLIMPORT_TOTAL_MS 900000U
#define WALLIMPORT_IDLE_MS 30000U
#define WALLIMPORT_TMP_NAME ".lock_wallpaper.rgb565.tmp"

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint16_t reserved;
    uint32_t data_bytes;
    uint32_t crc32;
    uint32_t reserved2;
} import_header_t;

typedef struct {
    int fd;
    uint32_t start;
    uint32_t last;
    uint8_t buf[512];
    size_t pos;
    size_t len;
} line_reader_t;

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

static bool read_line(line_reader_t *r, char *line, size_t cap)
{
    size_t out = 0;
    for (;;) {
        while (r->pos < r->len) {
            char c = (char)r->buf[r->pos++];
            if (c == '\r') continue;
            if (c == '\n') {
                if (out == 0) continue;
                line[out] = '\0';
                return true;
            }
            if (out + 1 >= cap) return false;
            line[out++] = c;
            r->last = xTaskGetTickCount();
        }
        ssize_t n = read(r->fd, r->buf, sizeof(r->buf));
        if (n > 0) {
            r->pos = 0;
            r->len = (size_t)n;
            r->last = xTaskGetTickCount();
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
        uint32_t now = xTaskGetTickCount();
        if ((uint32_t)(now - r->last) > pdMS_TO_TICKS(WALLIMPORT_IDLE_MS) ||
            (uint32_t)(now - r->start) > pdMS_TO_TICKS(WALLIMPORT_TOTAL_MS)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool verify_file(const char *path, uint32_t expected_crc)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    import_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header.magic, "PWL1", 4) != 0 || header.width != WALLPAPER_W ||
        header.height != WALLPAPER_H || header.format != 1U ||
        header.data_bytes != WALLPAPER_BYTES || header.crc32 != expected_crc) {
        fclose(f);
        return false;
    }
    uint8_t buf[512];
    uint32_t crc = 0xFFFFFFFFU;
    size_t left = WALLPAPER_BYTES;
    while (left > 0) {
        size_t take = left < sizeof(buf) ? left : sizeof(buf);
        if (fread(buf, 1, take, f) != take) {
            fclose(f);
            return false;
        }
        crc = wallimport_crc32_update(crc, buf, take);
        left -= take;
    }
    fclose(f);
    return ~crc == expected_crc;
}

static void import_error(const char *code)
{
    printf("WALLIMPORT ERR code=%s\n", code);
    fflush(stdout);
}

void panel_wallpaper_import_run(void)
{
    int fd = -1;
    int flags = -1;
    FILE *temp = NULL;
    bool created = false;
    bool ok = false;
    uint8_t chunk[WALLIMPORT_CHUNK_BYTES];
    char line[WALLIMPORT_LINE];
    char dir[192];
    char final_path[224];
    char temp_path[224];
    line_reader_t reader;

    if (!panel_files_card_ready()) {
        import_error("no_card");
        return;
    }
    const char *root = panel_files_root_path();
    if (root == NULL || root[0] == '\0') {
        import_error("no_root");
        return;
    }
    int n = snprintf(dir, sizeof(dir), "%s/panel/assets/lock", root);
    if (n <= 0 || (size_t)n >= sizeof(dir)) {
        import_error("path_too_long");
        return;
    }
    n = snprintf(final_path, sizeof(final_path), "%s/lock_wallpaper.rgb565", dir);
    if (n <= 0 || (size_t)n >= sizeof(final_path)) {
        import_error("path_too_long");
        return;
    }
    n = snprintf(temp_path, sizeof(temp_path), "%s/%s", dir, WALLIMPORT_TMP_NAME);
    if (n <= 0 || (size_t)n >= sizeof(temp_path)) {
        import_error("path_too_long");
        return;
    }

    if (!panel_files_io_acquire()) {
        import_error("io_busy");
        return;
    }
    /* All existence checks and creation happen under the same lease that
     * prevents a files retry from unmounting the card mid-import. */
    if (file_exists(final_path)) {
        import_error("exists");
        goto done;
    }
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        import_error("mkdir_failed");
        goto done;
    }
    fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        import_error(errno == EEXIST ? "temp_exists" : "temp_open_failed");
        goto done;
    }
    created = true;
    temp = fdopen(fd, "wb");
    if (temp == NULL) {
        close(fd);
        fd = -1;
        import_error("temp_open_failed");
        goto done;
    }
    fd = -1;

    int stdin_fd = fileno(stdin);
    flags = fcntl(stdin_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(stdin_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        import_error("stdin_nonblock_failed");
        goto done;
    }

    printf("WALLIMPORT READY chunk=%u chunks=%u\n", (unsigned)WALLIMPORT_CHUNK_BYTES,
           (unsigned)WALLIMPORT_CHUNK_COUNT);
    fflush(stdout);

    reader.fd = stdin_fd;
    reader.start = xTaskGetTickCount();
    reader.last = reader.start;
    reader.pos = 0;
    reader.len = 0;

    if (!read_line(&reader, line, sizeof(line))) {
        import_error("begin_timeout");
        goto done;
    }
    uint32_t expected_crc = 0;
    if (!wallimport_parse_begin(line, &expected_crc)) {
        import_error("begin_invalid");
        goto done;
    }
    import_header_t header = {
        .magic = {'P', 'W', 'L', '1'},
        .width = WALLPAPER_W,
        .height = WALLPAPER_H,
        .format = 1,
        .reserved = 0,
        .data_bytes = WALLPAPER_BYTES,
        .crc32 = expected_crc,
        .reserved2 = 0,
    };
    if (fwrite(&header, 1, sizeof(header), temp) != sizeof(header)) {
        import_error("temp_write_failed");
        goto done;
    }
    printf("WALLIMPORT ACK BEGIN\n");
    fflush(stdout);

    uint32_t last_acked = UINT32_MAX;
    unsigned bad_attempts = 0;
    for (uint32_t expected = 0; expected < WALLIMPORT_CHUNK_COUNT;) {
        if (!read_line(&reader, line, sizeof(line))) {
            import_error("chunk_timeout");
            goto done;
        }
        if (strcmp(line, "WALLIMPORT ABORT") == 0) {
            import_error("sender_abort");
            goto done;
        }
        uint32_t seq = 0;
        uint32_t got_crc = 0;
        size_t len = 0;
        if (!wallimport_parse_data(line, &seq, &got_crc, chunk, &len) ||
            len != WALLIMPORT_CHUNK_BYTES) {
            import_error("bad_chunk");
            goto done;
        }
        if (seq == last_acked) {
            printf("WALLIMPORT ACK %u\n", (unsigned)seq);
            fflush(stdout);
            continue;
        }
        if (seq != expected ||
            wallimport_crc32(chunk, len) != got_crc) {
            if (++bad_attempts > 5) {
                import_error("too_many_bad_chunks");
                goto done;
            }
            printf("WALLIMPORT NACK %u\n", (unsigned)seq);
            fflush(stdout);
            continue;
        }
        if (fwrite(chunk, 1, len, temp) != len) {
            import_error("temp_write_failed");
            goto done;
        }
        bad_attempts = 0;
        last_acked = seq;
        expected++;
        printf("WALLIMPORT ACK %u\n", (unsigned)seq);
        fflush(stdout);
    }

    if (fflush(temp) != 0) {
        import_error("flush_failed");
        goto done;
    }
    int temp_fd = fileno(temp);
    if (fsync(temp_fd) != 0 && errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) {
        import_error("fsync_failed");
        goto done;
    }
    if (fclose(temp) != 0) {
        temp = NULL;
        import_error("close_failed");
        goto done;
    }
    temp = NULL;
    if (!verify_file(temp_path, expected_crc)) {
        import_error("verify_failed");
        goto done;
    }
    if (file_exists(final_path)) {
        import_error("exists");
        goto done;
    }
    if (rename(temp_path, final_path) != 0) {
        import_error("rename_failed");
        goto done;
    }
    created = false; /* renamed; no longer a temporary artifact */
    ok = true;

done:
    if (flags >= 0) (void)fcntl(fileno(stdin), F_SETFL, flags);
    if (temp != NULL) fclose(temp);
    if (!ok && created) remove(temp_path);
    panel_files_io_release();
    if (ok) {
        printf("WALLIMPORT OK path=%s bytes=%u\n", final_path, (unsigned)WALLPAPER_BYTES);
        fflush(stdout);
    }
}
