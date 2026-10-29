#pragma once

#include <stdbool.h>
#include <windows.h>

void fake_files_reset(void);
extern int fake_mount_result;
extern bool fake_read_block;
extern bool fake_read_fail;
extern HANDLE fake_read_started;
extern HANDLE fake_read_release;
extern int fake_unmount_calls;
