/*
 * aftrace - log when Canon's autofocus properties change, and from which task.
 *
 * Why this exists: the AF algorithm is an AEWB per-frame loop whose metric is
 * not reachable by string analysis (see docs/rom-6d-wifi-af.md). The one thing
 * that would point at the code is the *task* that delivers the AF result, and
 * ML can read that: get_current_task_name() inside a property handler tells us
 * which task wrote the property.
 *
 * This module writes nothing and calls no firmware address. It only registers
 * property handlers and logs.
 */
#include "dryos.h"
#include "module.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "fio-ml.h"

static FILE *logf;

static void alog(const char *fmt, ...)
{
    va_list ap;
    char buf[160];
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (logf) { FIO_WriteFile(logf, buf, n); FIO_WriteFile(logf, "\n", 1); }
}

#define AFTRACE_PROP(id, name) \
    static volatile uint32_t name##_val; \
    static uint32_t name##_last, name##_n; \
    PROP_HANDLER(id) { \
        name##_val = buf ? buf[0] : 0; name##_n++; \
        if (name##_val != name##_last) { \
            name##_last = name##_val; \
            alog("%s = %d  task=%s  t=%d", #id, (int)name##_val, \
                 get_current_task_name(), (int)get_ms_clock()); \
        } \
    }

/* PROP_LV_AF_RESULT: 0 focus ok, 1 failed; delivered at the end of AF */
AFTRACE_PROP(0x80050029u, afres)
/* brightness values the same component publishes, for the frame cadence */
AFTRACE_PROP(0x80050008u, lvbv)
AFTRACE_PROP(0x80050010u, bv)
/* the AF point block and mode, to see when a focus run starts */
AFTRACE_PROP(0x8000000Au, afpoint)
AFTRACE_PROP(0x80000004u, afmode)

static int alog_open(void)
{
    if (logf) return 0;
    logf = FIO_CreateFile("ML/LOGS/AFTRACE.LOG");
    return logf ? 0 : -1;
}

static void alog_close(void)
{
    if (logf) { FIO_CloseFile(logf); logf = NULL; }
}

static void atrace_task(void *unused)
{
    if (alog_open() != 0) return;
    alog("=== aftrace: watching AF props, task names in each line ===");
    msleep(30000);
    alog("=== aftrace: done, %d results, %d points, %d modes ===",
         (int)afres_n, (int)afpoint_n, (int)afmode_n);
    alog_close();
}

static void atrace_go(void *priv, int delta)
{
    task_create("aftrace", 0x1e, 0x1000, atrace_task, 0);
}

static struct menu_entry aftrace_menu[] =
{
    {
        .name = "Trace AF 30s",
        .select = atrace_go,
        .help = "Log every change of the AF result, AF point, AF mode and BV "
                "properties, with the name of the task that delivered it. "
                "Focus the camera while it runs, then read ML/LOGS/AFTRACE.LOG.",
    },
    MENU_EOL,
};

static unsigned int aftrace_init(void)
{
    menu_add("Debug", aftrace_menu, COUNT(aftrace_menu));
    return 0;
}

static unsigned int aftrace_deinit(void)
{
    alog_close();
    return 0;
}


MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(0x80050029u)
    MODULE_PROPHANDLER(0x80050008u)
    MODULE_PROPHANDLER(0x80050010u)
    MODULE_PROPHANDLER(0x8000000Au)
    MODULE_PROPHANDLER(0x80000004u)
MODULE_PROPHANDLERS_END()

MODULE_INFO_START()
    MODULE_INIT(aftrace_init)
    MODULE_DEINIT(aftrace_deinit)
MODULE_INFO_END()
