#include <stdio.h>
#include <stdlib.h>

#include "panel_music.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

void test_runtime_init(void);
void worker_test_fail_task_create_at(int index);

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int fail_index = atoi(argv[1]);
    test_runtime_init();
    worker_test_fail_task_create_at(fail_index);
    CHECK(panel_music_init() == ESP_ERR_NO_MEM);
    /* s_init_failed prevents duplicate player/index tasks on retry. */
    CHECK(panel_music_init() == ESP_ERR_NO_MEM);
    printf("INIT_TASK_FAIL_OK fail_index=%d retry_rejected=1\n", fail_index);
    return 0;
}
