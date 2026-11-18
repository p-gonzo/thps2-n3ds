/* n3ds_platform.c - New Nintendo 3DS process setup for the THPS2 recompilation.
 *
 * Built only for NINTENDO_3DS (CMakeLists.txt). Three things the desktop gets
 * from its OS and a .3dsx does not:
 *
 *  1. A large enough main-thread stack. libctru gives a .3dsx 32 KB unless
 *     __stacksize__ says otherwise; the recompiled BIOS and game nest deeply
 *     and the desktop runs them on an 8 MB stack. 1 MB is the starting point;
 *     PS1 threads get their own fiber stacks (psx_fiber.c).
 *
 *  2. A working directory. The runtime resolves game.toml, bios/, disc/ and
 *     saves relative to the current directory, the way it runs from
 *     build-release/ on a PC. A .3dsx starts wherever the loader leaves it, so
 *     move into the data folder (THPS2_N3DS_DATA_DIR, default sdmc:/3ds/thps2)
 *     before main. libctru mounts sdmc: before running constructors.
 *
 *  3. Somewhere for stdout/stderr to go. The runtime reports its startup and
 *     failures on stdout, as on a PC. Here both go to svcOutputDebugString:
 *     emulators log it (Azahar: Debug.Emulated) and a debugger can read it on
 *     hardware; with neither attached the call returns immediately.
 *
 * The runtime is built with the toolchain's default enum size, the same as
 * libctru, so libctru headers are safe to include here. (CTR-3D's game code
 * needs -fno-short-enums; this project does not.)
 */
#include <3ds.h>
#include <sys/iosupport.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <unwind.h>   /* _Unwind_Ptr */

#ifndef THPS2_N3DS_DATA_DIR
#define THPS2_N3DS_DATA_DIR "sdmc:/3ds/thps2"
#endif

u32 __stacksize__ = 1u * 1024u * 1024u;

/* C++ exception index lookup, correcting devkitARM's 3dsx.ld.
 *
 * The ARM unwinder finds a frame's handler through the .ARM.exidx table,
 * bracketed by __exidx_start / __exidx_end. 3dsx.ld sets
 *     __exidx_start = .;
 *     .ARM.exidx : { ... }
 * outside the section, so when .ARM.extab ends off a 4-byte boundary the
 * symbol lands on the alignment padding (this build: 0x0175f1fa for a table
 * at 0x0175f1fc). Every 8-byte entry is then read 2 bytes early, no handler is
 * ever found, and any throw - even one caught a frame up - ends in
 * std::terminate.
 *
 * libgcc asks __gnu_Unwind_Find_exidx (a weak hook) for the table when one is
 * defined. The table is 4-byte aligned and the padding is under 4 bytes, so
 * rounding the start up to 4 recovers the true start exactly (and is a no-op
 * when there is no padding). */
extern const char __exidx_start[];
extern const char __exidx_end[];

_Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *nrec)
{
    (void)pc;
    const uintptr_t start = ((uintptr_t)__exidx_start + 3u) & ~(uintptr_t)3u;
    *nrec = (int)(((uintptr_t)__exidx_end - start) / 8u);
    return (_Unwind_Ptr)start;
}

/* libctru's default linear heap is 32 MB. The New 3DS gives an application
 * 124 MB and this program's code and static data take about 97 MB, so the
 * default split leaves nothing for the normal heap and libctru stops before
 * main. Linear memory is only for GPU/DSP-visible buffers: SDL's framebuffers
 * (about 1.3 MB for both screens) and audio. The rest becomes the normal heap. */
u32 __ctru_linear_heap_size = 8u * 1024u * 1024u;

/* One line at process exit, so a run that ends without a message from the
 * runtime still shows that it ended normally rather than crashed. */
static void thps2_n3ds_note_exit(void)
{
    static const char bye[] = "thps2-n3ds: process exit";
    svcOutputDebugString(bye, sizeof(bye) - 1);
}

__attribute__((constructor))
static void thps2_n3ds_process_setup(void)
{
    consoleDebugInit(debugDevice_SVC);            /* stderr -> svcOutputDebugString */
    devoptab_list[STD_OUT] = devoptab_list[STD_ERR];
    /* Not a terminal, so newlib would fully buffer: a hang or crash would
     * then lose exactly the lines that explain it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    static const char hello[] = "thps2-n3ds: process setup done";
    svcOutputDebugString(hello, sizeof(hello) - 1);
    printf("thps2-n3ds: stdout reaches the debug log\n");
    atexit(thps2_n3ds_note_exit);
    /* If the folder is missing, the runtime reports the missing game.toml /
     * BIOS / disc on its own; nothing to add here. */
    (void)chdir(THPS2_N3DS_DATA_DIR);

    /* Opt-in: an empty file named wait-for-debugger in the data folder holds
     * the process here for 15 s so a debugger (Azahar's GDB stub, or GDB over
     * Luma3DS on hardware) can attach before anything else runs. */
    if (access("wait-for-debugger", F_OK) == 0) {
        static const char wait[] = "thps2-n3ds: waiting 15 s for a debugger";
        svcOutputDebugString(wait, sizeof(wait) - 1);
        svcSleepThread(15LL * 1000 * 1000 * 1000);
    }
}
