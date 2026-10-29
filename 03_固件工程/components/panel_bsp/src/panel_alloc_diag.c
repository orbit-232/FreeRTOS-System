/*
 * Opt-in, read-only allocator diagnostic for the Hosted SDIO mempool failure.
 *
 * Built only when -DPANEL_NETWORK_ALLOC_DIAGNOSTICS=ON.  It makes no policy
 * decision and performs no fallback: every call is passed to the real
 * heap_caps_aligned_alloc and the exact return value is preserved.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

extern void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);

static volatile unsigned s_calls;
static volatile int s_in_wrapper;

static void log_call(unsigned n, size_t alignment, size_t size, uint32_t caps, void *ptr)
{
    if (n > 16 && ptr != NULL) {
        return;
    }
    const uint32_t internal_dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const uint32_t spiram_dma = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    const uint32_t spiram = MALLOC_CAP_SPIRAM;
    esp_rom_printf(
        "PNALLOC n=%u ok=%d align=%u size=%u caps=0x%08x "
        "intdma_free=%u intdma_largest=%u spdma_free=%u spdma_largest=%u "
        "spiram_free=%u spiram_largest=%u\n",
        n, ptr != NULL, (unsigned)alignment, (unsigned)size, (unsigned)caps,
        (unsigned)heap_caps_get_free_size(internal_dma),
        (unsigned)heap_caps_get_largest_free_block(internal_dma),
        (unsigned)heap_caps_get_free_size(spiram_dma),
        (unsigned)heap_caps_get_largest_free_block(spiram_dma),
        (unsigned)heap_caps_get_free_size(spiram),
        (unsigned)heap_caps_get_largest_free_block(spiram));
}

void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    /* Nested calls (including any call made by the ROM logger) must only pass
     * through; never log recursively. */
    if (__atomic_exchange_n(&s_in_wrapper, 1, __ATOMIC_ACQ_REL) != 0) {
        return __real_heap_caps_aligned_alloc(alignment, size, caps);
    }
    unsigned n = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
    void *ptr = __real_heap_caps_aligned_alloc(alignment, size, caps);
    log_call(n, alignment, size, caps, ptr);
    __atomic_store_n(&s_in_wrapper, 0, __ATOMIC_RELEASE);
    return ptr;
}
