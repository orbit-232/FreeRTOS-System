#include "panel_bsp.h"

bool panel_bsp_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void panel_bsp_unlock(void)
{
}

lv_indev_t *panel_bsp_input(void)
{
    return NULL;
}

lv_display_t *panel_bsp_display(void)
{
    return lv_display_get_default();
}
