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

/* Rung 2.5: write the Wi-Fi on/off property once Watch Props has named it.
 * Off by default, like the code-call rung: property_write is a normal ML
 * export, but the *id* has to be the one the firmware really uses, and a wrong
 * id means writing an unrelated setting. */
CONFIG_INT("mlwifi.switch_id", mlw_switch_id, 0);
CONFIG_INT("mlwifi.switch_value", mlw_switch_value, 1);
CONFIG_INT("mlwifi.allow_write", mlw_allow_write, 0);

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
#define MLW_PROP_LOGICAL_CONNECT 0x8003000Eu
#define MLW_PROP_CONNECT_TARGET  0x80030016u

/* Budget, not a guess. ML keeps handlers in property_handlers[256] and
 * prop_add_handler() had no bounds check (fixed in src/property.c so it can
 * never happen again). ML core already registers 149, so a module may add 107.
 * The 129-id version this replaced pushed the total to 278 and corrupted memory
 * past the array on every boot. 49 ids keeps the total at 198, with room for
 * another module to grow.
 *
 * 0x80030000..0x2F covers everything the firmware's network dispatcher
 * (0xFF284FA0) and the live log have shown so far: 0x0E, 0x0F, 0x16, plus the
 * housekeeping ids 0x14, 0x1D, 0x24. */
#define MLW_NIDS 116u
#define MLW_BUDGET_OK 1

/* One handler per id, registered through the module's property table so ML can
 * unregister them on unload (prop_unregister_slave is not exported). Each body
 * is one assignment plus one increment: this runs in PropMgrTask. */
#define MLW_PROP_INT(id, name, idx) \
    static volatile uint32_t name; \
    PROP_HANDLER(id) { name = buf ? buf[0] : 0; mlw_hits[idx]++; }

static volatile uint32_t mlw_hits[MLW_NIDS];

MLW_PROP_INT(0x80030000u, mlw_v0, 0)
MLW_PROP_INT(0x80030001u, mlw_v1, 1)
MLW_PROP_INT(0x80030002u, mlw_v2, 2)
MLW_PROP_INT(0x80030003u, mlw_v3, 3)
MLW_PROP_INT(0x80030004u, mlw_v4, 4)
MLW_PROP_INT(0x80030005u, mlw_v5, 5)
MLW_PROP_INT(0x80030006u, mlw_v6, 6)
MLW_PROP_INT(0x80030007u, mlw_v7, 7)
MLW_PROP_INT(0x80030008u, mlw_v8, 8)
MLW_PROP_INT(0x80030009u, mlw_v9, 9)
MLW_PROP_INT(0x8003000Au, mlw_v10, 10)
MLW_PROP_INT(0x8003000Bu, mlw_v11, 11)
MLW_PROP_INT(0x8003000Cu, mlw_v12, 12)
MLW_PROP_INT(0x8003000Du, mlw_v13, 13)
MLW_PROP_INT(0x8003000Eu, mlw_v14, 14)
MLW_PROP_INT(0x8003000Fu, mlw_v15, 15)
MLW_PROP_INT(0x80030010u, mlw_v16, 16)
MLW_PROP_INT(0x80030011u, mlw_v17, 17)
MLW_PROP_INT(0x80030012u, mlw_v18, 18)
MLW_PROP_INT(0x80030013u, mlw_v19, 19)
MLW_PROP_INT(0x80030014u, mlw_v20, 20)
MLW_PROP_INT(0x80030015u, mlw_v21, 21)
MLW_PROP_INT(0x80030016u, mlw_v22, 22)
MLW_PROP_INT(0x80030017u, mlw_v23, 23)
MLW_PROP_INT(0x80030018u, mlw_v24, 24)
MLW_PROP_INT(0x80030019u, mlw_v25, 25)
MLW_PROP_INT(0x8003001Au, mlw_v26, 26)
MLW_PROP_INT(0x8003001Bu, mlw_v27, 27)
MLW_PROP_INT(0x8003001Cu, mlw_v28, 28)
MLW_PROP_INT(0x8003001Du, mlw_v29, 29)
MLW_PROP_INT(0x8003001Eu, mlw_v30, 30)
MLW_PROP_INT(0x8003001Fu, mlw_v31, 31)
MLW_PROP_INT(0x80030020u, mlw_v32, 32)
MLW_PROP_INT(0x80030021u, mlw_v33, 33)
MLW_PROP_INT(0x80030022u, mlw_v34, 34)
MLW_PROP_INT(0x80030023u, mlw_v35, 35)
MLW_PROP_INT(0x80030024u, mlw_v36, 36)
MLW_PROP_INT(0x80030025u, mlw_v37, 37)
MLW_PROP_INT(0x80030026u, mlw_v38, 38)
MLW_PROP_INT(0x80030027u, mlw_v39, 39)
MLW_PROP_INT(0x80030028u, mlw_v40, 40)
MLW_PROP_INT(0x80030029u, mlw_v41, 41)
MLW_PROP_INT(0x8003002Au, mlw_v42, 42)
MLW_PROP_INT(0x8003002Bu, mlw_v43, 43)
MLW_PROP_INT(0x8003002Cu, mlw_v44, 44)
MLW_PROP_INT(0x8003002Du, mlw_v45, 45)
MLW_PROP_INT(0x8003002Eu, mlw_v46, 46)
MLW_PROP_INT(0x8003002Fu, mlw_v47, 47)
MLW_PROP_INT(0x80010000u, mlw_v48, 48)
MLW_PROP_INT(0x80040001u, mlw_v49, 49)
MLW_PROP_INT(0x80040002u, mlw_v50, 50)
MLW_PROP_INT(0x80040003u, mlw_v51, 51)
MLW_PROP_INT(0x80040004u, mlw_v52, 52)
MLW_PROP_INT(0x80040005u, mlw_v53, 53)
MLW_PROP_INT(0x80040006u, mlw_v54, 54)
MLW_PROP_INT(0x80040007u, mlw_v55, 55)
MLW_PROP_INT(0x8004000Fu, mlw_v56, 56)
MLW_PROP_INT(0x80040011u, mlw_v57, 57)
MLW_PROP_INT(0x8004001Au, mlw_v58, 58)
MLW_PROP_INT(0x8004001Cu, mlw_v59, 59)
MLW_PROP_INT(0x8004001Du, mlw_v60, 60)
MLW_PROP_INT(0x80040021u, mlw_v61, 61)
MLW_PROP_INT(0x80040022u, mlw_v62, 62)
MLW_PROP_INT(0x80040023u, mlw_v63, 63)
MLW_PROP_INT(0x80040024u, mlw_v64, 64)
MLW_PROP_INT(0x80040025u, mlw_v65, 65)
MLW_PROP_INT(0x80040026u, mlw_v66, 66)
MLW_PROP_INT(0x80040027u, mlw_v67, 67)
MLW_PROP_INT(0x80040028u, mlw_v68, 68)
MLW_PROP_INT(0x80040029u, mlw_v69, 69)
MLW_PROP_INT(0x8004002Au, mlw_v70, 70)
MLW_PROP_INT(0x8004002Bu, mlw_v71, 71)
MLW_PROP_INT(0x8004002Cu, mlw_v72, 72)
MLW_PROP_INT(0x8004002Du, mlw_v73, 73)
MLW_PROP_INT(0x8004002Eu, mlw_v74, 74)
MLW_PROP_INT(0x8004002Fu, mlw_v75, 75)
MLW_PROP_INT(0x80040030u, mlw_v76, 76)
MLW_PROP_INT(0x80040031u, mlw_v77, 77)
MLW_PROP_INT(0x80040032u, mlw_v78, 78)
MLW_PROP_INT(0x80040033u, mlw_v79, 79)
MLW_PROP_INT(0x80040034u, mlw_v80, 80)
MLW_PROP_INT(0x80040035u, mlw_v81, 81)
MLW_PROP_INT(0x80040036u, mlw_v82, 82)
MLW_PROP_INT(0x80040037u, mlw_v83, 83)
MLW_PROP_INT(0x80040038u, mlw_v84, 84)
MLW_PROP_INT(0x80040039u, mlw_v85, 85)
MLW_PROP_INT(0x8004003Au, mlw_v86, 86)
MLW_PROP_INT(0x8004003Bu, mlw_v87, 87)
MLW_PROP_INT(0x8004003Cu, mlw_v88, 88)
MLW_PROP_INT(0x8004003Du, mlw_v89, 89)
MLW_PROP_INT(0x8004003Eu, mlw_v90, 90)
MLW_PROP_INT(0x8004003Fu, mlw_v91, 91)
MLW_PROP_INT(0x80040040u, mlw_v92, 92)
MLW_PROP_INT(0x80040041u, mlw_v93, 93)
MLW_PROP_INT(0x80040043u, mlw_v94, 94)
MLW_PROP_INT(0x80040044u, mlw_v95, 95)
MLW_PROP_INT(0x80040045u, mlw_v96, 96)
MLW_PROP_INT(0x80040046u, mlw_v97, 97)
MLW_PROP_INT(0x80040047u, mlw_v98, 98)
MLW_PROP_INT(0x80040048u, mlw_v99, 99)
MLW_PROP_INT(0x80040049u, mlw_v100, 100)
MLW_PROP_INT(0x8004004Au, mlw_v101, 101)
MLW_PROP_INT(0x8004004Bu, mlw_v102, 102)
MLW_PROP_INT(0x8004004Cu, mlw_v103, 103)
MLW_PROP_INT(0x8004004Du, mlw_v104, 104)
MLW_PROP_INT(0x8004004Eu, mlw_v105, 105)
MLW_PROP_INT(0x8004004Fu, mlw_v106, 106)
MLW_PROP_INT(0x80040050u, mlw_v107, 107)
MLW_PROP_INT(0x80040051u, mlw_v108, 108)
MLW_PROP_INT(0x80040052u, mlw_v109, 109)
MLW_PROP_INT(0x80040053u, mlw_v110, 110)
MLW_PROP_INT(0x80040056u, mlw_v111, 111)
MLW_PROP_INT(0x80040057u, mlw_v112, 112)
MLW_PROP_INT(0x80040058u, mlw_v113, 113)
MLW_PROP_INT(0x80040059u, mlw_v114, 114)
MLW_PROP_INT(0x8004005Au, mlw_v115, 115)

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
    &mlw_v64,
    &mlw_v65,
    &mlw_v66,
    &mlw_v67,
    &mlw_v68,
    &mlw_v69,
    &mlw_v70,
    &mlw_v71,
    &mlw_v72,
    &mlw_v73,
    &mlw_v74,
    &mlw_v75,
    &mlw_v76,
    &mlw_v77,
    &mlw_v78,
    &mlw_v79,
    &mlw_v80,
    &mlw_v81,
    &mlw_v82,
    &mlw_v83,
    &mlw_v84,
    &mlw_v85,
    &mlw_v86,
    &mlw_v87,
    &mlw_v88,
    &mlw_v89,
    &mlw_v90,
    &mlw_v91,
    &mlw_v92,
    &mlw_v93,
    &mlw_v94,
    &mlw_v95,
    &mlw_v96,
    &mlw_v97,
    &mlw_v98,
    &mlw_v99,
    &mlw_v100,
    &mlw_v101,
    &mlw_v102,
    &mlw_v103,
    &mlw_v104,
    &mlw_v105,
    &mlw_v106,
    &mlw_v107,
    &mlw_v108,
    &mlw_v109,
    &mlw_v110,
    &mlw_v111,
    &mlw_v112,
    &mlw_v113,
    &mlw_v114,
    &mlw_v115,
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
    0x80010000u,
    0x80040001u,
    0x80040002u,
    0x80040003u,
    0x80040004u,
    0x80040005u,
    0x80040006u,
    0x80040007u,
    0x8004000Fu,
    0x80040011u,
    0x8004001Au,
    0x8004001Cu,
    0x8004001Du,
    0x80040021u,
    0x80040022u,
    0x80040023u,
    0x80040024u,
    0x80040025u,
    0x80040026u,
    0x80040027u,
    0x80040028u,
    0x80040029u,
    0x8004002Au,
    0x8004002Bu,
    0x8004002Cu,
    0x8004002Du,
    0x8004002Eu,
    0x8004002Fu,
    0x80040030u,
    0x80040031u,
    0x80040032u,
    0x80040033u,
    0x80040034u,
    0x80040035u,
    0x80040036u,
    0x80040037u,
    0x80040038u,
    0x80040039u,
    0x8004003Au,
    0x8004003Bu,
    0x8004003Cu,
    0x8004003Du,
    0x8004003Eu,
    0x8004003Fu,
    0x80040040u,
    0x80040041u,
    0x80040043u,
    0x80040044u,
    0x80040045u,
    0x80040046u,
    0x80040047u,
    0x80040048u,
    0x80040049u,
    0x8004004Au,
    0x8004004Bu,
    0x8004004Cu,
    0x8004004Du,
    0x8004004Eu,
    0x8004004Fu,
    0x80040050u,
    0x80040051u,
    0x80040052u,
    0x80040053u,
    0x80040056u,
    0x80040057u,
    0x80040058u,
    0x80040059u,
    0x8004005Au,
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
        mlw_log("probe: %s 0x%08x first word 0x%08x %s",
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

/* Report only what changed, and prove the task ran to completion.
 *
 * A previous run left no "watch: done" line, so it was impossible to tell a
 * silent block from a task killed by power-off. Every 10th tick now logs a
 * heartbeat, and the loop always ends with a full dump of non-zero values. */
static void mlw_watch_task(void *unused)
{
    (void)unused;
    uint32_t shadow[MLW_NIDS];
    unsigned k;
    int i, changes = 0;

    for (k = 0; k < MLW_NIDS; k++)
        shadow[k] = mlw_ptrs[k] ? *mlw_ptrs[k] : 0;

    mlw_log("watch: begin, %d ids, %d already non-zero",
            MLW_NIDS, 0);

    for (k = 0; k < MLW_NIDS; k++)
    {
        if (shadow[k])
            mlw_log("watch: initial 0x%x = %d", (int)mlw_ids[k], (int)shadow[k]);
    }

    for (i = 0; i < 90; i++)           /* 90 x 2 s = 3 minutes */
    {
        msleep(2000);
        for (k = 0; k < MLW_NIDS; k++)
        {
            if (mlw_ptrs[k] && *mlw_ptrs[k] != shadow[k])
            {
                mlw_log("change: 0x%x %d -> %d",
                        (int)mlw_ids[k], (int)shadow[k], (int)*mlw_ptrs[k]);
                shadow[k] = mlw_ptrs[k] ? *mlw_ptrs[k] : 0;
                changes++;
            }
        }
        if (i % 5 == 4)
            mlw_log("watch: alive, %d s, %d changes", (i + 1) * 2, changes);
    }

    for (k = 0; k < MLW_NIDS; k++)
    {
        if (mlw_hits[k])
            mlw_log("delivered: 0x%x hits=%d last=%d",
                    (int)mlw_ids[k], (int)mlw_hits[k], (int)*mlw_ptrs[k]);
    }
    mlw_log("watch: done, %d changes total", changes);
}

static void mlw_watch(void *priv, int delta)
{
    (void)priv; (void)delta;
    task_create("mlw_watch", 0x1e, 0x1000, mlw_watch_task, 0);
}

/* Look a property id up in the watched tables. Returns 0 when the id is not
 * watched, which is distinguishable because index 0 is 0x80030000 and no
 * caller asks for it. */
static int mlw_index_of(unsigned id)
{
    unsigned k;
    for (k = 0; k < MLW_NIDS; k++)
        if (mlw_ids[k] == id)
            return (int)k;
    return -1;
}

static void mlw_dump_task(void *unused)
{
    (void)unused;
    unsigned k;
    int nz = 0, i_log = mlw_index_of(MLW_PROP_LOGICAL_CONNECT);
    int i_ct  = mlw_index_of(MLW_PROP_CONNECT_TARGET);

    mlw_log("state: on demand");
    for (k = 0; k < MLW_NIDS; k++)
    {
        if (*mlw_ptrs[k])
        {
            mlw_log("state: 0x%x = %d", (int)mlw_ids[k], (int)*mlw_ptrs[k]);
            nz++;
        }
    }
    mlw_log("state: %d non-zero", nz);

    NotifyBox(3000, "nz=%d log=%d ct=0x%x",
              nz,
              i_log >= 0 ? (int)*mlw_ptrs[i_log] : -1,
              i_ct >= 0 ? (int)*mlw_ptrs[i_ct] : -1);
}

/* Menu handlers must return immediately: no file I/O here. */
static void mlw_state_dump(void *priv, int delta)
{
    (void)priv; (void)delta;
    task_create("mlw_dump", 0x1e, 0x1000, mlw_dump_task, 0);
}


/* Enumerate DryOS tasks and log their names. Read-only: get_task_name_from_id
 * is a core ML export (6D_116.sym line 101), and it only reads the task table.
 * The point is to see whether the WLAN side is alive on this body: the ROM
 * carries wpaTask and dhcpsTask, so their presence means the WiFi stack has
 * been started, and their absence means it has not. */
static void mlw_tasks_task(void *unused)
{
    (void)unused;
    mlw_log("tasks: begin");
    int found = 0;
    for (int id = 0; id < 64; id++)
    {
        const char *n = get_task_name_from_id(id);
        if (n && n[0])
        {
            mlw_log("task %2d: %s", id, n);
            found++;
        }
    }
    mlw_log("tasks: done, %d named tasks", found);
}

static void mlw_tasks(void *priv, int delta)
{
    (void)priv; (void)delta;
    task_create("mlw_tasks", 0x1e, 0x1000, mlw_tasks_task, 0);
}


/* Write the configured property id with the configured value. This is the only
 * way a module can turn the Wi-Fi switch: modules cannot read properties
 * (prop_get_value is not exported), but prop_request_change is. */
static void mlw_write_switch(void *priv, int delta)
{
    (void)priv; (void)delta;
    if (!mlw_allow_write || !mlw_switch_id)
    {
        mlw_log("write: refused (allow_write=%d switch_id=0x%x)",
                (int)mlw_allow_write, (int)mlw_switch_id);
        NotifyBox(2000, "write refused: set allow_write and switch_id");
        return;
    }
    uint32_t val = (uint32_t)mlw_switch_value;
    prop_request_change((unsigned)mlw_switch_id, &val, sizeof(val));
    mlw_log("write: prop 0x%x <- %d requested", (int)mlw_switch_id,
            (int)mlw_switch_value);
    NotifyBox(2000, "write 0x%x = %d", (int)mlw_switch_id,
              (int)mlw_switch_value);
}

static struct menu_entry mlwifi_menu[] = {
    {
        .name = "Watch Props 3min",
        .select = mlw_watch,
        .icon_type = IT_ACTION,
        .help = "Watch the whole network block for three minutes: one line per "
                "change, a heartbeat every 10 s, and a final dump. "
                "Turn WiFi on or off in Canon's menu while it runs: the id "
                "whose value changes is the switch ML would have to write. "
                "Subscribing and logging write nothing, so this cannot harm "
                "the camera.",
    },
    {
        .name = "Dump State",
        .select = mlw_state_dump,
        .icon_type = IT_ACTION,
        .help = "Log every non-zero value in the network property block and pop "
                "a box with the two ids already tied to names: 0x8003000e "
                "(logical connect) and 0x80030016 (connect target; bit 0x40 = "
                "connected). Read-only. Lines go to ML/LOGS/MLWIFI.LOG and to "
                "the console.",
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
    {
        .name = "List Tasks",
        .select = mlw_tasks,
        .icon_type = IT_ACTION,
        .help = "Log every DryOS task name. The ROM has wpaTask and dhcpsTask, "
                "so if the WiFi stack is running they appear here; if they do "
                "not, the WLAN side was never started on this body. Read-only.",
    },
    {
        .name = "Write Switch",
        .select = mlw_write_switch,
        .icon_type = IT_ACTION,
        .help = "Rung 2.5: write mlwifi.switch_id with mlwifi.switch_value "
                "through prop_request_change, the normal ML property API. It "
                "only runs when mlwifi.allow_write is 1. Set switch_id to the "
                "id that Watch Props named when Wi-Fi was toggled in Canon's "
                "menu. Nothing here jumps into firmware code.",
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
    MODULE_PROPHANDLER(0x80010000u)
    MODULE_PROPHANDLER(0x80040001u)
    MODULE_PROPHANDLER(0x80040002u)
    MODULE_PROPHANDLER(0x80040003u)
    MODULE_PROPHANDLER(0x80040004u)
    MODULE_PROPHANDLER(0x80040005u)
    MODULE_PROPHANDLER(0x80040006u)
    MODULE_PROPHANDLER(0x80040007u)
    MODULE_PROPHANDLER(0x8004000Fu)
    MODULE_PROPHANDLER(0x80040011u)
    MODULE_PROPHANDLER(0x8004001Au)
    MODULE_PROPHANDLER(0x8004001Cu)
    MODULE_PROPHANDLER(0x8004001Du)
    MODULE_PROPHANDLER(0x80040021u)
    MODULE_PROPHANDLER(0x80040022u)
    MODULE_PROPHANDLER(0x80040023u)
    MODULE_PROPHANDLER(0x80040024u)
    MODULE_PROPHANDLER(0x80040025u)
    MODULE_PROPHANDLER(0x80040026u)
    MODULE_PROPHANDLER(0x80040027u)
    MODULE_PROPHANDLER(0x80040028u)
    MODULE_PROPHANDLER(0x80040029u)
    MODULE_PROPHANDLER(0x8004002Au)
    MODULE_PROPHANDLER(0x8004002Bu)
    MODULE_PROPHANDLER(0x8004002Cu)
    MODULE_PROPHANDLER(0x8004002Du)
    MODULE_PROPHANDLER(0x8004002Eu)
    MODULE_PROPHANDLER(0x8004002Fu)
    MODULE_PROPHANDLER(0x80040030u)
    MODULE_PROPHANDLER(0x80040031u)
    MODULE_PROPHANDLER(0x80040032u)
    MODULE_PROPHANDLER(0x80040033u)
    MODULE_PROPHANDLER(0x80040034u)
    MODULE_PROPHANDLER(0x80040035u)
    MODULE_PROPHANDLER(0x80040036u)
    MODULE_PROPHANDLER(0x80040037u)
    MODULE_PROPHANDLER(0x80040038u)
    MODULE_PROPHANDLER(0x80040039u)
    MODULE_PROPHANDLER(0x8004003Au)
    MODULE_PROPHANDLER(0x8004003Bu)
    MODULE_PROPHANDLER(0x8004003Cu)
    MODULE_PROPHANDLER(0x8004003Du)
    MODULE_PROPHANDLER(0x8004003Eu)
    MODULE_PROPHANDLER(0x8004003Fu)
    MODULE_PROPHANDLER(0x80040040u)
    MODULE_PROPHANDLER(0x80040041u)
    MODULE_PROPHANDLER(0x80040043u)
    MODULE_PROPHANDLER(0x80040044u)
    MODULE_PROPHANDLER(0x80040045u)
    MODULE_PROPHANDLER(0x80040046u)
    MODULE_PROPHANDLER(0x80040047u)
    MODULE_PROPHANDLER(0x80040048u)
    MODULE_PROPHANDLER(0x80040049u)
    MODULE_PROPHANDLER(0x8004004Au)
    MODULE_PROPHANDLER(0x8004004Bu)
    MODULE_PROPHANDLER(0x8004004Cu)
    MODULE_PROPHANDLER(0x8004004Du)
    MODULE_PROPHANDLER(0x8004004Eu)
    MODULE_PROPHANDLER(0x8004004Fu)
    MODULE_PROPHANDLER(0x80040050u)
    MODULE_PROPHANDLER(0x80040051u)
    MODULE_PROPHANDLER(0x80040052u)
    MODULE_PROPHANDLER(0x80040053u)
    MODULE_PROPHANDLER(0x80040056u)
    MODULE_PROPHANDLER(0x80040057u)
    MODULE_PROPHANDLER(0x80040058u)
    MODULE_PROPHANDLER(0x80040059u)
    MODULE_PROPHANDLER(0x8004005Au)
MODULE_PROPHANDLERS_END()

MODULE_INFO_START()
    MODULE_INIT(mlwifi_init)
    MODULE_DEINIT(mlwifi_deinit)
MODULE_INFO_END()
