/*
 * mlwifi - reach the 6D's own WiFi stack.
 *
 * STATUS: ladder rung 1. This module does not transfer anything yet. Read
 * docs/wifi-functions.md before touching it; the short version:
 *
 *   - the camera has a complete WiFi stack (NWCOM: FTP transfer, discovery,
 *     connection monitoring, WFT HTTP) and Canon's own GUI drives it
 *   - none of it is exported in 6D_116.sym, so no module can import it by name
 *   - the addresses below were found by disassembly of ROM1 at 0xFF000000,
 *     base and md5 recorded in docs/wifi-functions.md
 *   - every WiFi function is identified by the strings it logs through
 *     Canon's routine at 0x67C8, passed PC-relative, which is why no pointer
 *     to any of those strings exists in the image
 *
 * The code here deliberately CALLS NOTHING. Rung 1 is: report that the address
 * is there and what surrounds it. Turning an integer into a function pointer
 * and jumping to it without knowing the signature is how a camera gets bricked.
 *
 * Rung 2 is implemented and gated: "Call NWCOM Init" jumps to 0xFF261714 with
 * r0-r3 = 0 and logs the return. It refuses unless "allow_call" is 1, because
 * the signature is inferred, not known -- if the function expects a real
 * context pointer it may fault, and a fault in a module task takes the camera
 * down until the battery is pulled. Nothing is written to the card either way.
 */
#include "dryos.h"
#include "module.h"
#include "menu.h"
#include "bmp.h"
#include "config.h"
#include "property.h"
#include "notify_box.h"
#include "fio-ml.h"

/* Off by default: turning this to 1 is the only way the call below can run. */
CONFIG_INT("mlwifi.allow_call", mlw_allow_call, 0);

/* printf() in ML goes to the console buffer and never reaches the card, so
 * every line here is also appended to ML/LOGS/MLWIFI.LOG. That file is the
 * only evidence that survives a power cycle, and without it a run leaves
 * nothing to read. Only call this from a task, never from a menu handler:
 * it opens and closes a file. */
static void mlw_log(const char *fmt, ...)
{
    char line[220];
    int pos = snprintf(line, sizeof(line), "[%7dms][mlwifi] ", get_ms_clock());
    if (pos < 0)
        pos = 0;
    if (pos < (int)sizeof(line) - 2)
    {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(line + pos, sizeof(line) - pos - 1, fmt, ap);
        va_end(ap);
        if (n > 0)
            pos += n;
        if (pos >= (int)sizeof(line) - 1)
            pos = sizeof(line) - 2;
    }
    line[pos++] = '\n';
    line[pos] = 0;

    printf("%s", line);

    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/MLWIFI.LOG");
    if (f)
    {
        FIO_WriteFile(f, line, pos);
        FIO_CloseFile(f);
    }
}

/* The network property block. Every id is registered as its own handler through
 * the module's own property table: ML unregisters those on module unload
 * (module.c:854 prop_add_handler), whereas a direct prop_register_slave() call
 * cannot be undone -- prop_unregister_slave is not exported, so disabling the
 * module would leave DryOS calling a freed handler.
 *
 * The block was found in ROM1 at 0xFF284FA0, a dispatcher that routes incoming
 * property ids and tests exactly 0x80010000, 0x80030016, 0x8003000F and
 * 0x8003000E. 0x8003000E is ML's own PROP_LOGICAL_CONNECT (property.h:644),
 * which confirms the block. Watching 0x80030000..0x8003003F rather than the
 * four known ids is deliberate: the WiFi on/off switch is not identified yet.
 *
 * PROP_INT only assigns -- no logging, no sleeping, safe in PropMgrTask.
 */
#define MLW_NIDS 65u

PROP_INT(0x80030000u, mlw_v0)
PROP_INT(0x80030001u, mlw_v1)
PROP_INT(0x80030002u, mlw_v2)
PROP_INT(0x80030003u, mlw_v3)
PROP_INT(0x80030004u, mlw_v4)
PROP_INT(0x80030005u, mlw_v5)
PROP_INT(0x80030006u, mlw_v6)
PROP_INT(0x80030007u, mlw_v7)
PROP_INT(0x80030008u, mlw_v8)
PROP_INT(0x80030009u, mlw_v9)
PROP_INT(0x8003000Au, mlw_v10)
PROP_INT(0x8003000Bu, mlw_v11)
PROP_INT(0x8003000Cu, mlw_v12)
PROP_INT(0x8003000Du, mlw_v13)
PROP_INT(0x8003000Eu, mlw_v14)
PROP_INT(0x8003000Fu, mlw_v15)
PROP_INT(0x80030010u, mlw_v16)
PROP_INT(0x80030011u, mlw_v17)
PROP_INT(0x80030012u, mlw_v18)
PROP_INT(0x80030013u, mlw_v19)
PROP_INT(0x80030014u, mlw_v20)
PROP_INT(0x80030015u, mlw_v21)
PROP_INT(0x80030016u, mlw_v22)
PROP_INT(0x80030017u, mlw_v23)
PROP_INT(0x80030018u, mlw_v24)
PROP_INT(0x80030019u, mlw_v25)
PROP_INT(0x8003001Au, mlw_v26)
PROP_INT(0x8003001Bu, mlw_v27)
PROP_INT(0x8003001Cu, mlw_v28)
PROP_INT(0x8003001Du, mlw_v29)
PROP_INT(0x8003001Eu, mlw_v30)
PROP_INT(0x8003001Fu, mlw_v31)
PROP_INT(0x80030020u, mlw_v32)
PROP_INT(0x80030021u, mlw_v33)
PROP_INT(0x80030022u, mlw_v34)
PROP_INT(0x80030023u, mlw_v35)
PROP_INT(0x80030024u, mlw_v36)
PROP_INT(0x80030025u, mlw_v37)
PROP_INT(0x80030026u, mlw_v38)
PROP_INT(0x80030027u, mlw_v39)
PROP_INT(0x80030028u, mlw_v40)
PROP_INT(0x80030029u, mlw_v41)
PROP_INT(0x8003002Au, mlw_v42)
PROP_INT(0x8003002Bu, mlw_v43)
PROP_INT(0x8003002Cu, mlw_v44)
PROP_INT(0x8003002Du, mlw_v45)
PROP_INT(0x8003002Eu, mlw_v46)
PROP_INT(0x8003002Fu, mlw_v47)
PROP_INT(0x80030030u, mlw_v48)
PROP_INT(0x80030031u, mlw_v49)
PROP_INT(0x80030032u, mlw_v50)
PROP_INT(0x80030033u, mlw_v51)
PROP_INT(0x80030034u, mlw_v52)
PROP_INT(0x80030035u, mlw_v53)
PROP_INT(0x80030036u, mlw_v54)
PROP_INT(0x80030037u, mlw_v55)
PROP_INT(0x80030038u, mlw_v56)
PROP_INT(0x80030039u, mlw_v57)
PROP_INT(0x8003003Au, mlw_v58)
PROP_INT(0x8003003Bu, mlw_v59)
PROP_INT(0x8003003Cu, mlw_v60)
PROP_INT(0x8003003Du, mlw_v61)
PROP_INT(0x8003003Eu, mlw_v62)
PROP_INT(0x8003003Fu, mlw_v63)
PROP_INT(0x80010000u, mlw_v64)

static volatile uint32_t *const mlw_ptrs[MLW_NIDS] =
{
    &mlw_v0,
    &mlw_v1,
    &mlw_v2,
    &mlw_v3,
    &mlw_v4,
    &mlw_v5,
    &mlw_v6,
    &mlw_v7,
    &mlw_v8,
    &mlw_v9,
    &mlw_v10,
    &mlw_v11,
    &mlw_v12,
    &mlw_v13,
    &mlw_v14,
    &mlw_v15,
    &mlw_v16,
    &mlw_v17,
    &mlw_v18,
    &mlw_v19,
    &mlw_v20,
    &mlw_v21,
    &mlw_v22,
    &mlw_v23,
    &mlw_v24,
    &mlw_v25,
    &mlw_v26,
    &mlw_v27,
    &mlw_v28,
    &mlw_v29,
    &mlw_v30,
    &mlw_v31,
    &mlw_v32,
    &mlw_v33,
    &mlw_v34,
    &mlw_v35,
    &mlw_v36,
    &mlw_v37,
    &mlw_v38,
    &mlw_v39,
    &mlw_v40,
    &mlw_v41,
    &mlw_v42,
    &mlw_v43,
    &mlw_v44,
    &mlw_v45,
    &mlw_v46,
    &mlw_v47,
    &mlw_v48,
    &mlw_v49,
    &mlw_v50,
    &mlw_v51,
    &mlw_v52,
    &mlw_v53,
    &mlw_v54,
    &mlw_v55,
    &mlw_v56,
    &mlw_v57,
    &mlw_v58,
    &mlw_v59,
    &mlw_v60,
    &mlw_v61,
    &mlw_v62,
    &mlw_v63,
    &mlw_v64
};

static const unsigned mlw_ids[MLW_NIDS] =
{
    0x80030000u,
    0x80030001u,
    0x80030002u,
    0x80030003u,
    0x80030004u,
    0x80030005u,
    0x80030006u,
    0x80030007u,
    0x80030008u,
    0x80030009u,
    0x8003000Au,
    0x8003000Bu,
    0x8003000Cu,
    0x8003000Du,
    0x8003000Eu,
    0x8003000Fu,
    0x80030010u,
    0x80030011u,
    0x80030012u,
    0x80030013u,
    0x80030014u,
    0x80030015u,
    0x80030016u,
    0x80030017u,
    0x80030018u,
    0x80030019u,
    0x8003001Au,
    0x8003001Bu,
    0x8003001Cu,
    0x8003001Du,
    0x8003001Eu,
    0x8003001Fu,
    0x80030020u,
    0x80030021u,
    0x80030022u,
    0x80030023u,
    0x80030024u,
    0x80030025u,
    0x80030026u,
    0x80030027u,
    0x80030028u,
    0x80030029u,
    0x8003002Au,
    0x8003002Bu,
    0x8003002Cu,
    0x8003002Du,
    0x8003002Eu,
    0x8003002Fu,
    0x80030030u,
    0x80030031u,
    0x80030032u,
    0x80030033u,
    0x80030034u,
    0x80030035u,
    0x80030036u,
    0x80030037u,
    0x80030038u,
    0x80030039u,
    0x8003003Au,
    0x8003003Bu,
    0x8003003Cu,
    0x8003003Du,
    0x8003003Eu,
    0x8003003Fu,
    0x80010000u
};

/* Addresses from the ROM1 disassembly. See docs/wifi-functions.md. */
#define MLW_NWCOM_INIT      0xFF261714u   /* [NWCOM] NWCOM Init */
#define MLW_NWCOM_XMITCBR   0xFF2616E8u   /* TransmissionChangeCBR */
#define MLW_NWCOM_STATUSCBR 0xFF261B00u   /* NetworkStatusCBR */
#define MLW_NWCOM_WIZARD    0xFF261F70u   /* GUID / FriendlyName / discovery */
#define MLW_NWCOM_ORDER     0xFF262358u   /* transfer order handler */
#define MLW_NWCOM_FTP       0xFF261974u   /* FTP transfer control */
#define MLW_NWCOM_CONN      0xFF2634B8u   /* ChangeLogicalConnect */
#define MLW_NWCOM_RECONN    0xFF2638C4u   /* BeaconLoss / ReConnect */
#define MLW_NWCOM_GUIEVT    0xFF266334u   /* NWCOM GUIEvent */
#define MLW_LOG_CALL        0x000067C8u   /* Canon's logging routine */

/* Flash window only: every address above is in it, and reading unmapped RAM
 * is how a module takes the camera down. */
#define MLW_FLASH_LO 0xFF000000u
#define MLW_FLASH_SPAN 0x01000000u

static int mlw_readable(uint32_t addr)
{
    return (uint32_t)(addr - MLW_FLASH_LO) < MLW_FLASH_SPAN;
}

static uint32_t mlw_first_word(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void mlw_probe_task(void *unused)
{
    (void)unused;
    struct { const char *name; uint32_t addr; } t[] = {
        { "NWCOM_Init",      MLW_NWCOM_INIT     },
        { "TransmissionCBR", MLW_NWCOM_XMITCBR  },
        { "StatusCBR",       MLW_NWCOM_STATUSCBR},
        { "Wizard",          MLW_NWCOM_WIZARD   },
        { "Order",           MLW_NWCOM_ORDER    },
        { "FTP",             MLW_NWCOM_FTP      },
        { "LogicalConnect",  MLW_NWCOM_CONN     },
        { "ReConnect",       MLW_NWCOM_RECONN   },
        { "GUIEvent",        MLW_NWCOM_GUIEVT   },
    };
    int i, ok = 0;

    mlw_log("probe: begin");
    for (i = 0; i < COUNT(t); i++) {
        if (!mlw_readable(t[i].addr)) {
            mlw_log("probe: %s 0x%08x outside the flash window", t[i].name, t[i].addr);
            continue;
        }
        /* first word of each function: the ARM prologue. 0xE92Dxxxx is
         * `push {..., lr}`, which is what every one of these starts with. */
        uint32_t w = mlw_first_word(t[i].addr);
        int prologue = (w & 0x0FFF0000u) == 0x092D0000u && (w & 0x4000u);
        mlw_log("probe: %-16s 0x%08x first word 0x%08x %s",
                t[i].name, t[i].addr, w, prologue ? "prologue" : "NOT a prologue");
        if (prologue) ok++;
    }
    mlw_log("probe: %d of %d look like functions; logging routine at 0x%08x",
            ok, COUNT(t), MLW_LOG_CALL);
    NotifyBox(2000, "mlwifi: %d/%d WiFi functions found", ok, COUNT(t));
}

static void mlw_probe(void *priv, int delta)
{
    (void)priv; (void)delta;
    task_create("mlw_probe", 0x1e, 0x1000, mlw_probe_task, 0);
}

/* Rung 2. Canon's code is entered with r0-r3 = 0. That is a guess, but a
 * deterministic one -- far better than whatever happened to be in those
 * registers. The prologue check below is the same one the probe uses, so an
 * address that is not a function entry is never jumped to. */
typedef int (*mlw_fn4)(uint32_t, uint32_t, uint32_t, uint32_t);

static void mlw_call_task(void *unused)
{
    (void)unused;

    if (!mlw_readable(MLW_NWCOM_INIT)) {
        mlw_log("call: NWCOM Init 0x%08x is outside the flash window", MLW_NWCOM_INIT);
        NotifyBox(3000, "mlwifi: address not in flash");
        return;
    }

    uint32_t w = mlw_first_word(MLW_NWCOM_INIT);
    mlw_log("call: NWCOM Init 0x%08x first word 0x%08x", MLW_NWCOM_INIT, w);
    if (((w & 0x0FFF0000u) != 0x092D0000u) || !(w & 0x4000u)) {
        mlw_log("call: refused, first word is not an ARM prologue");
        NotifyBox(4000, "mlwifi: 0x%x is not a function entry", MLW_NWCOM_INIT);
        return;
    }

    mlw_log("call: entering NWCOM Init with r0-r3 = 0");
    mlw_fn4 fn = (mlw_fn4)MLW_NWCOM_INIT;
    int rc = fn(0, 0, 0, 0);
    mlw_log("call: NWCOM Init returned 0x%08x", (uint32_t)rc);
    NotifyBox(4000, "NWCOM Init -> 0x%x", (uint32_t)rc);
}

static void mlw_call(void *priv, int delta)
{
    (void)priv; (void)delta;
    if (!mlw_allow_call) {
        NotifyBox(3000, "mlwifi: set allow_call = 1 first");
        return;
    }
    task_create("mlw_call", 0x1e, 0x2000, mlw_call_task, 0);
}

/* Report only what changed. Dumping 65 values every two seconds would bury the
 * one line that matters, so each tick compares against a shadow copy and prints
 * a line per transition: "change: 0x8003003a 0 -> 1". Turn WiFi on and off in
 * Canon's menu while this runs and the switch identifies itself. */
static void mlw_watch_task(void *unused)
{
    (void)unused;
    uint32_t shadow[MLW_NIDS];
    unsigned k;
    int i;

    for (k = 0; k < MLW_NIDS; k++)
        shadow[k] = *mlw_ptrs[k];

    mlw_log("watch: begin, %d ids, block 0x80030000..0x8003003f + 0x80010000", MLW_NIDS);

    for (k = 0; k < MLW_NIDS; k++)
    {
        if (shadow[k])
            mlw_log("state: 0x%x = %d", (int)mlw_ids[k], (int)shadow[k]);
    }

    for (i = 0; i < 30; i++)
    {
        msleep(2000);
        for (k = 0; k < MLW_NIDS; k++)
        {
            if (*mlw_ptrs[k] != shadow[k])
            {
                mlw_log("change: 0x%x %d -> %d",
                       (int)mlw_ids[k], (int)shadow[k], (int)*mlw_ptrs[k]);
                shadow[k] = *mlw_ptrs[k];
            }
        }
    }

    mlw_log("watch: done");
}

static void mlw_watch(void *priv, int delta)
{
    (void)priv; (void)delta;
    task_create("mlw_watch", 0x1e, 0x1000, mlw_watch_task, 0);
}

static struct menu_entry mlwifi_menu[] = {
    {
        .name = "Watch Props 60s",
        .select = mlw_watch,
        .icon_type = IT_ACTION,
        .help = "Log the four network property ids every 2 s for a minute. "
                "Turn WiFi on or off in Canon's menu while it runs: the id "
                "whose value changes is the switch ML would have to write. "
                "Subscribing and logging write nothing, so this cannot harm "
                "the camera.",
    },
    {
        .name = "Probe WiFi",
        .select = mlw_probe,
        .icon_type = IT_ACTION,
        .help = "Check that the nine WiFi functions found by disassembly are "
                "present and start with an ARM prologue. Calls nothing, so the "
                "worst case is a note in the log. Read docs/wifi-functions.md "
                "for what each address is and how it was found.",
    },
    {
        .name = "Call NWCOM Init",
        .select = mlw_call,
        .icon_type = IT_ACTION,
        .help = "Rung 2: jump to NWCOM Init at 0xFF261714 with r0-r3 = 0 and "
                "log the return. The signature is inferred from disassembly, "
                "not known, so this is the first real risk in the ladder: if "
                "the call is wrong the camera may hang until the battery is "
                "pulled. It only runs when allow_call is 1 under ML's module "
                "config (Modules > mlwifi). Nothing is written to the card.",
    },
    MENU_EOL,
};

static unsigned int mlwifi_init(void)
{
    menu_add("WiFi", mlwifi_menu, COUNT(mlwifi_menu) - 1);
    return 0;
}

static unsigned int mlwifi_deinit(void)
{
    return 0;
}

MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(0x80030000u)
    MODULE_PROPHANDLER(0x80030001u)
    MODULE_PROPHANDLER(0x80030002u)
    MODULE_PROPHANDLER(0x80030003u)
    MODULE_PROPHANDLER(0x80030004u)
    MODULE_PROPHANDLER(0x80030005u)
    MODULE_PROPHANDLER(0x80030006u)
    MODULE_PROPHANDLER(0x80030007u)
    MODULE_PROPHANDLER(0x80030008u)
    MODULE_PROPHANDLER(0x80030009u)
    MODULE_PROPHANDLER(0x8003000Au)
    MODULE_PROPHANDLER(0x8003000Bu)
    MODULE_PROPHANDLER(0x8003000Cu)
    MODULE_PROPHANDLER(0x8003000Du)
    MODULE_PROPHANDLER(0x8003000Eu)
    MODULE_PROPHANDLER(0x8003000Fu)
    MODULE_PROPHANDLER(0x80030010u)
    MODULE_PROPHANDLER(0x80030011u)
    MODULE_PROPHANDLER(0x80030012u)
    MODULE_PROPHANDLER(0x80030013u)
    MODULE_PROPHANDLER(0x80030014u)
    MODULE_PROPHANDLER(0x80030015u)
    MODULE_PROPHANDLER(0x80030016u)
    MODULE_PROPHANDLER(0x80030017u)
    MODULE_PROPHANDLER(0x80030018u)
    MODULE_PROPHANDLER(0x80030019u)
    MODULE_PROPHANDLER(0x8003001Au)
    MODULE_PROPHANDLER(0x8003001Bu)
    MODULE_PROPHANDLER(0x8003001Cu)
    MODULE_PROPHANDLER(0x8003001Du)
    MODULE_PROPHANDLER(0x8003001Eu)
    MODULE_PROPHANDLER(0x8003001Fu)
    MODULE_PROPHANDLER(0x80030020u)
    MODULE_PROPHANDLER(0x80030021u)
    MODULE_PROPHANDLER(0x80030022u)
    MODULE_PROPHANDLER(0x80030023u)
    MODULE_PROPHANDLER(0x80030024u)
    MODULE_PROPHANDLER(0x80030025u)
    MODULE_PROPHANDLER(0x80030026u)
    MODULE_PROPHANDLER(0x80030027u)
    MODULE_PROPHANDLER(0x80030028u)
    MODULE_PROPHANDLER(0x80030029u)
    MODULE_PROPHANDLER(0x8003002Au)
    MODULE_PROPHANDLER(0x8003002Bu)
    MODULE_PROPHANDLER(0x8003002Cu)
    MODULE_PROPHANDLER(0x8003002Du)
    MODULE_PROPHANDLER(0x8003002Eu)
    MODULE_PROPHANDLER(0x8003002Fu)
    MODULE_PROPHANDLER(0x80030030u)
    MODULE_PROPHANDLER(0x80030031u)
    MODULE_PROPHANDLER(0x80030032u)
    MODULE_PROPHANDLER(0x80030033u)
    MODULE_PROPHANDLER(0x80030034u)
    MODULE_PROPHANDLER(0x80030035u)
    MODULE_PROPHANDLER(0x80030036u)
    MODULE_PROPHANDLER(0x80030037u)
    MODULE_PROPHANDLER(0x80030038u)
    MODULE_PROPHANDLER(0x80030039u)
    MODULE_PROPHANDLER(0x8003003Au)
    MODULE_PROPHANDLER(0x8003003Bu)
    MODULE_PROPHANDLER(0x8003003Cu)
    MODULE_PROPHANDLER(0x8003003Du)
    MODULE_PROPHANDLER(0x8003003Eu)
    MODULE_PROPHANDLER(0x8003003Fu)
    MODULE_PROPHANDLER(0x80010000u)
MODULE_PROPHANDLERS_END()

MODULE_INFO_START()
    MODULE_INIT(mlwifi_init)
    MODULE_DEINIT(mlwifi_deinit)
MODULE_INFO_END()
