#include "panel_boot_key.h"

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    panel_boot_key_t key;
    panel_boot_key_init(&key, true, 0, 1500);
    CHECK(!panel_boot_key_update(&key, true, 800));
    CHECK(!panel_boot_key_update(&key, true, 1700)); /* still held */
    CHECK(!panel_boot_key_update(&key, false, 1720));
    CHECK(!panel_boot_key_update(&key, false, 1760)); /* released; arm now */
    CHECK(!panel_boot_key_update(&key, false, 1800));
    CHECK(!panel_boot_key_update(&key, true, 1900));
    CHECK(panel_boot_key_update(&key, true, 1940)); /* first press */
    CHECK(!panel_boot_key_update(&key, true, 2100)); /* held: no repeat */
    CHECK(!panel_boot_key_update(&key, false, 2200));
    CHECK(!panel_boot_key_update(&key, false, 2240));
    CHECK(!panel_boot_key_update(&key, true, 2300));
    CHECK(panel_boot_key_update(&key, true, 2340)); /* next press */

    panel_boot_key_init(&key, false, 0, 1500);
    CHECK(!panel_boot_key_update(&key, false, 500));
    CHECK(!panel_boot_key_update(&key, true, 900)); /* before arm, consumed */
    CHECK(!panel_boot_key_update(&key, false, 950));
    CHECK(!panel_boot_key_update(&key, false, 1600));
    CHECK(!panel_boot_key_update(&key, true, 1700));
    CHECK(panel_boot_key_update(&key, true, 1740)); /* post-arm press */
    printf("BOOT_KEY_TESTS_OK\n");
    return 0;
}
