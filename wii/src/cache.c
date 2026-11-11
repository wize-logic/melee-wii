/* The SDK's cache calls under their SDK names: libogc 3.1 has them as
 * tuxedo's (PPCDCache*), its own DC* being inline or renamed. libogc's
 * headers only here: the SDK's declare these names again. */

#include <stddef.h>
#include <stdint.h>

#include <tuxedo/ppc/cache.h>

void DCFlushRange(void* addr, uint32_t nBytes)
{
    PPCDCacheFlush(addr, nBytes);
}

void DCFlushRangeNoSync(void* addr, uint32_t nBytes)
{
    PPCDCacheFlushAsync(addr, nBytes);
}

void DCStoreRange(void* addr, uint32_t nBytes)
{
    PPCDCacheStore(addr, nBytes);
}

void DCStoreRangeNoSync(void* addr, uint32_t nBytes)
{
    PPCDCacheStoreAsync(addr, nBytes);
}

void DCInvalidateRange(void* addr, uint32_t nBytes)
{
    PPCDCacheInvalidate(addr, nBytes);
}

void DCZeroRange(void* addr, uint32_t nBytes)
{
    PPCDCacheZero(addr, nBytes);
}

void DCTouchRange(void* addr, uint32_t nBytes)
{
    PPCDCacheTouch(addr, nBytes);
}

void ICInvalidateRange(void* addr, uint32_t nBytes)
{
    PPCICacheInvalidate(addr, nBytes);
}
