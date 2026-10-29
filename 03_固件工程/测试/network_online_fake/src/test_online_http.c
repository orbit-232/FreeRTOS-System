#include "panel_online_http.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "fake_http.h"

static int failures;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void begin(void) { fake_http_reset(); }

static void test_success_known_length(void)
{
    begin();
    const char *json = "{\"ok\":true}";
    fake_http_set_body(json, strlen(json));
    fake_http_set_content_length((int64_t)strlen(json));
    fake_http_set_content_type("application/json; charset=utf-8");
    char body[128];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_OK);
    CHECK(status == 200);
    CHECK(len == strlen(json));
    CHECK(strcmp(body, json) == 0);
    CHECK(strstr(type, "application/json") != NULL);
    CHECK(fake_http_open_calls() == 1);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_content_length_oversize(void)
{
    begin();
    fake_http_set_body("12345678901234567890", 20);
    fake_http_set_content_length(20);
    char body[16];
    size_t len = 99;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_INVALID_SIZE);
    CHECK(fake_http_read_calls() == 0);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_chunked_oversize(void)
{
    begin();
    char payload[200];
    memset(payload, 'a', sizeof(payload));
    fake_http_set_body(payload, sizeof(payload));
    fake_http_set_content_length(-1);
    fake_http_set_chunk_size(8);
    char body[32];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_INVALID_SIZE);
    CHECK(fake_http_read_calls() > 0);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_bad_type_and_status(void)
{
    begin();
    fake_http_set_body("hello", 5);
    fake_http_set_content_length(5);
    fake_http_set_content_type("text/html");
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_INVALID_RESPONSE);
    CHECK(fake_http_read_calls() == 0);

    begin();
    fake_http_set_status(500);
    fake_http_set_body("error", 5);
    fake_http_set_content_length(5);
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_OK);
    CHECK(status == 500);
    CHECK(len == 0);
    CHECK(fake_http_read_calls() == 0);
    CHECK(fake_http_close_calls() == 1);
}

static void test_read_error(void)
{
    begin();
    char payload[100];
    memset(payload, 'b', sizeof(payload));
    fake_http_set_body(payload, sizeof(payload));
    fake_http_set_content_length((int64_t)sizeof(payload));
    fake_http_set_read_error_after(10);
    char body[128];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_FAIL);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_trickle_deadline(void)
{
    begin();
    fake_http_set_body("", 0);
    fake_http_set_content_length(-1);
    fake_http_set_chunk_size(1);
    fake_http_set_chunk_delay_ms(10);
    fake_http_set_endless(true);
    char body[128];
    size_t len = 0;
    int status = 0;
    char type[64];
    DWORD start = GetTickCount();
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_TIMEOUT);
    DWORD elapsed = GetTickCount() - start;
    CHECK(elapsed < 3000);
    CHECK(fake_http_read_calls() > 0);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_eagain(void)
{
    begin();
    fake_http_set_body("", 0);
    fake_http_set_content_length(-1);
    fake_http_set_read_eagain(true);
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_TIMEOUT);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_open_failure(void)
{
    begin();
    fake_http_set_open_error(ESP_FAIL);
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_FAIL);
    CHECK(fake_http_open_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
}

static void test_single_read_block_cancelled(void)
{
    begin();
    fake_http_set_body("", 0);
    fake_http_set_content_length(-1);
    fake_http_set_read_block(true);
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    DWORD start = GetTickCount();
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_TIMEOUT);
    DWORD elapsed = GetTickCount() - start;
    CHECK(elapsed < 3000);
    CHECK(fake_http_shutdown_calls() == 1);
    CHECK(fake_http_internal_packets() >= 2);
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);
    CHECK(fake_http_cleanup_after_shutdown());
}

static void test_fetch_headers_block_cancelled(void)
{
    begin();
    fake_http_set_fetch_block(true);
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    DWORD start = GetTickCount();
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_ERR_TIMEOUT);
    DWORD elapsed = GetTickCount() - start;
    CHECK(elapsed < 3000);
    CHECK(fake_http_shutdown_calls() == 1);
    CHECK(fake_http_internal_packets() >= 2);
    CHECK(fake_http_cleanup_after_shutdown());
}

static void test_delayed_shutdown_join_and_next_request(void)
{
    begin();
    fake_http_set_body("", 0);
    fake_http_set_content_length(-1);
    fake_http_set_read_block(true);
    fake_http_set_shutdown_delay_ms(600);
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    DWORD start = GetTickCount();
    CHECK(panel_online_http_get("https://example.test/slow", body, sizeof(body), &len,
                                &status, type, sizeof(type)) == ESP_ERR_TIMEOUT);
    DWORD elapsed = GetTickCount() - start;
    CHECK(elapsed >= 550); /* longer than the old 200 ms disarm poll */
    CHECK(fake_http_shutdown_calls() == 1);
    CHECK(!fake_http_close_before_shutdown_complete());
    CHECK(fake_http_cleanup_after_shutdown());
    CHECK(fake_http_close_calls() == 1);
    CHECK(fake_http_cleanup_calls() == 1);

    /* A stale fd/shutdown must not affect the next request on a new socket. */
    fake_http_set_body("{}", 2);
    fake_http_set_content_length(2);
    fake_http_set_content_type("application/json");
    fake_http_set_read_block(false);
    fake_http_set_socket(4243);
    CHECK(panel_online_http_get("https://example.test/next", body, sizeof(body), &len,
                                &status, type, sizeof(type)) == ESP_OK);
    CHECK(status == 200);
    CHECK(len == 2);
    CHECK(fake_http_shutdown_calls() == 1);
    CHECK(fake_http_close_calls() == 2);
    CHECK(fake_http_cleanup_calls() == 2);
}

static void test_exact_cap_minus_one(void)
{
    begin();
    char payload[63];
    memset(payload, 'c', sizeof(payload));
    fake_http_set_body(payload, sizeof(payload));
    fake_http_set_content_length((int64_t)sizeof(payload));
    char body[64];
    size_t len = 0;
    int status = 0;
    char type[64];
    CHECK(panel_online_http_get("https://example.test/x", body, sizeof(body), &len, &status,
                                type, sizeof(type)) == ESP_OK);
    CHECK(len == sizeof(payload));
    CHECK(body[len] == '\0');
}

int main(void)
{
    test_success_known_length();
    test_content_length_oversize();
    test_chunked_oversize();
    test_bad_type_and_status();
    test_read_error();
    test_trickle_deadline();
    test_eagain();
    test_open_failure();
    test_single_read_block_cancelled();
    test_fetch_headers_block_cancelled();
    test_delayed_shutdown_join_and_next_request();
    test_exact_cap_minus_one();
    printf("ONLINE_HTTP_TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
