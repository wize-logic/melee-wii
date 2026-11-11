/* The GameCube's 16 MiB of ARAM: the Wii has none. It is a block of MEM2
 * here; an ARAM address is an offset into it, as the game counts them. */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/ar.h>
#include <dolphin/os.h>

#include <wl/platform/platform.h>

#define ARAM_SIZE (16 * 1024 * 1024)
/* The SDK's ARInit keeps the first 16 KiB for the system */
#define ARAM_USER_BASE 0x4000

static u8* s_aram;
static u32 s_stack_pointer;
static u32* s_stack_index;
static u32 s_stack_entries;
static u32 s_stack_count;

u8* melee_wii_aram(void)
{
    return s_aram;
}

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    if (s_aram != NULL) {
        return ARAM_USER_BASE;
    }
    s_aram = memalign(32, ARAM_SIZE); /* newlib's malloc is in MEM2 */
    if (s_aram == NULL) {
        melee_wii_fatal("ARInit: no 16 MiB of MEM2 for ARAM");
    }
    memset(s_aram, 0, ARAM_SIZE);
    DCFlushRange(s_aram, ARAM_SIZE);
    OSReport("AR: the game's ARAM is 16 MiB of MEM2 at %p\n", s_aram);
    if (wl_platform_fs_exists("sd:/apps/melee/crashtest")) {
        /* tests: a crash here, for stall.c's report */
        OSReport("AR: crashtest: a trap\n");
        __builtin_trap();
    }
    if (wl_platform_fs_exists("sd:/apps/melee/stalltest")) {
        /* tests: a start that stops here, for stall.c's report */
        static OSThreadQueue never;
        OSReport("AR: stalltest: waiting for good\n");
        OSInitThreadQueue(&never);
        OSSleepThread(&never);
    }
    s_stack_pointer = ARAM_USER_BASE;
    s_stack_index = stack_index_addr;
    s_stack_entries = num_entries;
    s_stack_count = 0;
    return ARAM_USER_BASE;
}

int ARCheckInit(void)
{
    return s_aram != NULL;
}

u32 ARAlloc(u32 length)
{
    BOOL enabled = OSDisableInterrupts();
    u32 addr = s_stack_pointer;
    s_stack_pointer += length;
    if (s_stack_index != NULL && s_stack_count < s_stack_entries) {
        s_stack_index[s_stack_count] = length;
    }
    s_stack_count++;
    OSRestoreInterrupts(enabled);
    return addr;
}

u32 ARFree(u32* length)
{
    BOOL enabled = OSDisableInterrupts();
    if (s_stack_count > 0) {
        s_stack_count--;
        if (s_stack_index != NULL && s_stack_count < s_stack_entries) {
            u32 freed = s_stack_index[s_stack_count];
            s_stack_pointer -= freed;
            if (length != NULL) {
                *length = freed;
            }
        }
    }
    u32 addr = s_stack_pointer;
    OSRestoreInterrupts(enabled);
    return addr;
}

u32 ARGetBaseAddress(void)
{
    return ARAM_USER_BASE;
}

u32 ARGetSize(void)
{
    return ARAM_SIZE;
}

static void copy(u32 type, u32 mainmem, u32 aram, u32 length)
{
    if (aram >= ARAM_SIZE || length > ARAM_SIZE - aram) {
        OSReport("AR: transfer of 0x%x bytes at ARAM 0x%x is outside it\n",
                 (unsigned) length, (unsigned) aram);
        return;
    }
    /* A main memory address is physical or cached: either way the cached
     * mapping of the same bytes */
    u8* main = (u8*) ((mainmem & 0x3FFFFFFF) | 0x80000000);
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        memcpy(s_aram + aram, main, length);
        /* The DSP reads ARAM's samples from memory, past the CPU's cache */
        DCFlushRange(s_aram + aram, length);
    } else {
        memcpy(main, s_aram + aram, length);
        melee_wii_note_load((u32) main, length);
        /* As DMA would leave it: in memory, whatever the game then does with
         * the lines over it */
        DCFlushRange(main, length);
    }
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    copy(type, mainmem_addr, aram_addr, length);
}

u32 ARGetDMAStatus(void)
{
    return 0;
}

/* arq.c: the transfers are copies, made where the ARAM interrupt would
 * have come (melee_wii_defer), one request each time, its callback after */
static ARQRequest* s_head;
static ARQRequest* s_tail;
static MeleeWiiDefer s_defer;
static u32 s_chunk_size = 0x1000;

static void arq_run(void* arg)
{
    (void) arg;
    ARQRequest* request = s_head;
    if (request == NULL) {
        return;
    }
    s_head = request->next;
    if (s_head == NULL) {
        s_tail = NULL;
    }
    if (request->type == ARQ_TYPE_MRAM_TO_ARAM) {
        copy(request->type, request->source, request->dest, request->length);
    } else {
        copy(request->type, request->dest, request->source, request->length);
    }
    if (request->callback != NULL) {
        request->callback(request);
    }
    if (s_head != NULL) {
        melee_wii_defer(&s_defer);
    }
}

void ARQInit(void)
{
    s_defer.fn = arq_run;
    s_defer.arg = NULL;
}

void ARQPostRequest(ARQRequest* request, uintptr_t owner, u32 type,
                    u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback)
{
    request->next = NULL;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;

    BOOL enabled = OSDisableInterrupts();
    if (s_tail != NULL) {
        s_tail->next = request;
    } else {
        s_head = request;
    }
    s_tail = request;
    s_defer.fn = arq_run;
    melee_wii_defer(&s_defer);
    OSRestoreInterrupts(enabled);
}

void ARQRemoveRequest(ARQRequest* request)
{
    BOOL enabled = OSDisableInterrupts();
    ARQRequest** link = &s_head;
    ARQRequest* prev = NULL;
    while (*link != NULL) {
        if (*link == request) {
            *link = request->next;
            if (s_tail == request) {
                s_tail = prev;
            }
            break;
        }
        prev = *link;
        link = &(*link)->next;
    }
    OSRestoreInterrupts(enabled);
}

void ARQSetChunkSize(u32 size)
{
    s_chunk_size = size;
}

u32 ARQGetChunkSize(void)
{
    return s_chunk_size;
}
