# Two camera tests that close the open questions

Everything still unknown about the AF algorithm and about Wi-Fi on this body needs
one run each. Both use modules that are already in `magiclantern.zip` and both are
read-only in every path they take. Nothing here calls a firmware address.

Before either test: install the zip, enable the module (Modules → aftrace /
mlwifi), power-cycle, and keep the card where you can read `ML/LOGS/`.

## Test 1 — which task publishes the AF result (closes the AF algorithm)

`aftrace` watches five properties and records, for each change, the value, the
**task name** and the millisecond clock:

```
0x80050029  PROP_LV_AF_RESULT    0 = focus ok, 1 = failed
0x80050027  PROP_LV_FOCUS_CMD    the lens drive command
0x80050002  PROP_LV_FOCUS_DONE   drive finished
0x80050008  PROP_LV_BV           LiveView brightness
0x80050010  PROP_BV              brightness
```

Procedure:

1. Debug → **Trace AF 30s**.
2. During those 30 s, do both kinds of focus, a few times each:
   * Canon's own AF (half-press, or the AF button), and
   * ML's Focus menu steps (+ and −), which go through `lens_focus()` and
     `PROP_LV_LENS_DRIVE_REMOTE`.
3. Send `ML/LOGS/AFTRACE.LOG`.

What each outcome means:

| log shows | reading |
|---|---|
| `0x80050029` lines with a task name | that task is the AF result producer: its code is where the evaluation ends, and its address is findable from the task table |
| `0x80050027` and `0x80050002` lines but no `0x80050029` | the drive loop is visible but the result never propagates as a property on this body; the decision is internal to the task |
| no lines at all | the properties are not delivered to ML's property manager on this body — check the boot line in `AFTRACE.LOG` first, then report it as a negative |

## Test 2 — is the developed JPEG reachable by Canon's app (closes Wi-Fi)

No module needed. Develop already writes `DCIM/<dir>/IMG_nnnn.JPG` and replaces
the camera's own JPEG at that name (`film.sim.replace` defaults to 1), so from the
camera's point of view the file is the one it wrote.

Procedure:

1. Turn Wi-Fi on **in Canon's own menu** (this is the part ML does not yet drive).
2. Connect the phone app and open the file list.
3. Note three things:
   * does `IMG_nnnn.JPG` for your developed shot appear,
   * does the transferred file carry the film-sim look (not the camera's JPEG),
   * is the matching `.CR2` listed as well.

This is the "works today" path from `docs/wifi-recipes-plan.md`; the `.CR2`
question has been open since 2026-11-06 and costs one look.

## What I would not do

`mlwifi` → **Call NWCOM Init** stays gated. `NWCOM Init` dereferences its `r0`
(`ldr r3, [r4, #32]` at `0xFF26175C`) and is a component event handler, so calling
it with zeros is a data abort, not an experiment.

`mlwifi` → **Watch Props 3min** is still worth a run for a different reason: if
toggling Wi-Fi in Canon's menu produces *any* `change:` line, that names a
property ML can watch to know the radio state, even if nothing needs writing.
