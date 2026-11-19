/* n3ds_fatal.cpp - say why the program stopped, on the console itself.
 *
 * An uncaught C++ exception ends in std::terminate, which on the 3DS just
 * closes the app: no message, back to the Homebrew Launcher. The likeliest
 * cause here is running out of memory (std::bad_alloc). This terminate
 * handler names the error in the debug log and on the bottom screen, then
 * waits for START (or HOME) before exiting.
 */
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>

#include "n3ds_hud.h"

extern "C" FILE *g_n3ds_log;
extern "C" void n3ds_log_flush(void);

namespace {

[[noreturn]] void n3ds_terminate()
{
    char detail[160] = "An unexpected error stopped the game.";
    const char *title = "FATAL ERROR";
    if (std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::bad_alloc &) {
            title = "OUT OF MEMORY";
            std::snprintf(detail, sizeof detail,
                          "The game needed more memory than this launch has. Heap: %lu of %lu MB in use.",
                          (unsigned long)(n3ds_heap_used() >> 20),
                          (unsigned long)(envGetHeapSize() >> 20));
        } catch (const std::exception &e) {
            std::snprintf(detail, sizeof detail, "%s", e.what());
        } catch (...) {
        }
    }
    char log[220];
    std::snprintf(log, sizeof log, "thps2-n3ds: %s: %s", title, detail);
    svcOutputDebugString(log, std::strlen(log));
    if (g_n3ds_log) { std::fprintf(g_n3ds_log, "%s\n", log); n3ds_log_flush(); }

    /* Before the first present SDL may not have set up the screens yet. */
    if (!g_n3ds_gfx_ready) {
        /* The same 32-bit format SDL's 3DS driver sets up, which
         * n3ds_hud_message draws in (gfxInitDefault would pick 24-bit). */
        gfxInit(GSP_RGBA8_OES, GSP_RGBA8_OES, false);
        hidInit();
    }
    for (int i = 0; i < 2; i++) {   /* both buffers of the double-buffered screen */
        n3ds_hud_message(title, detail, "PRESS START TO EXIT");
        gspWaitForVBlank();
    }
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    std::_Exit(EXIT_FAILURE);
}

struct InstallTerminate {
    InstallTerminate() { std::set_terminate(n3ds_terminate); }
} s_install_terminate;

/* Test switch: an empty file named test-fatal in the data folder raises an
 * out-of-memory error during start-up, before any graphics are set up, to
 * check this screen. (Absolute path: the data-folder chdir may not have run
 * yet at static-initialisation time.) */
struct TestFatal {
    TestFatal() {
        if (FILE *f = std::fopen("sdmc:/3ds/thps2/test-fatal", "rb")) {
            std::fclose(f);
            throw std::bad_alloc();
        }
    }
} s_test_fatal;

}  // namespace
