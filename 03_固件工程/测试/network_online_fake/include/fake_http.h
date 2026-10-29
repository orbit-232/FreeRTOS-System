#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void fake_http_reset(void);
void fake_http_set_status(int status);
void fake_http_set_content_type(const char *content_type);
void fake_http_set_content_length(int64_t length);
void fake_http_set_body(const char *body, size_t len);
void fake_http_set_chunk_size(size_t chunk_size);
void fake_http_set_chunk_delay_ms(unsigned delay_ms);
void fake_http_set_endless(bool endless);
void fake_http_set_open_error(esp_err_t error);
void fake_http_set_fetch_error(int64_t error);
void fake_http_set_read_error_after(int bytes);
void fake_http_set_read_eagain(bool eagain);
void fake_http_set_read_block(bool block);
void fake_http_set_fetch_block(bool block);
void fake_http_set_shutdown_delay_ms(unsigned delay_ms);
void fake_http_set_socket(int socket_fd);

int fake_http_open_calls(void);
int fake_http_close_calls(void);
int fake_http_cleanup_calls(void);
int fake_http_read_calls(void);
int fake_http_shutdown_calls(void);
bool fake_http_cleanup_after_shutdown(void);
bool fake_http_close_before_shutdown_complete(void);
int fake_http_internal_packets(void);
int fake_http_last_timeout_ms(void);

#ifdef __cplusplus
}
#endif
