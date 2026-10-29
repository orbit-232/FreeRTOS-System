#pragma once

#define SHUT_RDWR 2

#ifdef __cplusplus
extern "C" {
#endif

int fake_http_shutdown(int socket, int how);
#define shutdown fake_http_shutdown

#ifdef __cplusplus
}
#endif
