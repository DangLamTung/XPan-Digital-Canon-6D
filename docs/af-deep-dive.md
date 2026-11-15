# AF deep dive: where it runs, what drives the motor, what can be improved

Everything below is read from the firmware image, from ML's symbol table, or from
ML's own source. Where something is not established it says so.

## Where it runs

Two autofocus paths exist on this body, and they are different machines:

| path | sensor | where the code is |
|---|---|---|
| viewfinder | dedicated phase-difference AF sensor (11 points) | phase-difference calibration strings `PhaseDiffCarrierWave*`, `dl_SetDutyAFor*`; the measurement itself is not in a module I could identify |
| LiveView ("TVAF") | the LiveView image | `AEWB` subsystem, below |

The AEWB subsystem, by its own source-path strings:

| unit | address | role |
|---|---|---|
| `AEWB/AF/AFProc/LiveViewAFPUtility.c` | `0xFF4324C4` | LiveView AF utility: state, records, logging (194 functions in the window; none computes a pixel metric) |
| `AEWB/AECalc/AEProc/LiveViewAEProcMetering.c` | `0xFF5CB0E0` | metering over 16-byte zone records, `0xFF5C4A5C` |
| `AEWB/AECalc/AECalc.c` | `0xFF427354` | the arithmetic: weighted sum with a 1/1024 scale at `0xFF42EF18` |
| `AEWB/AEWBCommon/AEWBPropControl.c` | `0xFFD03BE4` | the property bridge: `AeWbRequestChangeLensDriveStartCBR` |
| `AEWB/AEWBCommon/AEWBDataStocker.c` | `0xFFCFE7C0` | the store of AE/AF values |

The controller publishes its own vocabulary through trace strings
(`AfCtrl_Act_*`, `AfCtrl_ExecuteEvent(Event, State)`), which is what makes it
readable as a state machine even without symbols.

The **task** that executes all this is the one thing not yet pinned: ML cannot see
it statically, because task names are reached by index. `modules/aftrace` exists to
print it.

## What drives the motor

The chain, from a module's point of view:

```
ML: lens_focus(n, stepsize, wait, delay)                  src/lens.c:755
    -> prop_request_change(PROP_LV_LENS_DRIVE_REMOTE, &focus_cmd, 4)   lens.c:808
       focus_cmd = stepsize (-3..3);  if negative: 0x8000 - stepsize   lens.c:781

firmware: PROP_LV_LENS_DRIVE_REMOTE = 0x80050013
    -> AEWBPropControl.c  AeWbRequestChangeLensDriveStartCBR
    -> AfCtrl action: Act_StartLensDriveRemote
    -> Act_EmdDriveResult( Comp:%d )        (drive in progress / done)
    -> Act_CompleteAfResult
    -> PROP_LV_FOCUS_DONE   = 0x80050002
    -> PROP_LV_AF_RESULT    = 0x80050029   (0 = focus ok, 1 = failed)
```

So **the command that moves the motor from ML is a property write**: `0x80050013`
with a signed step in the low bits. `PROP_LV_FOCUS_CMD` (`0x80050027`) is the
sibling used by the camera's own focus commands.

What is *not* identified: the layer that turns that request into serial traffic on
the EF mount contacts. It is not in ML's 250 stubs, and no source-path string in
the image names it. Its commands are therefore not readable from here.

One more piece of the puzzle, from `platform/6D.116/stubs.S:349`:

```
///NSTUB(0xFF1304DC, _prop_get_value)   // unsafe?
```

`prop_get_value` **is** known to ML (address `0xFF1304DC`, inside the property
system at `0xFF13xxxx`) but deliberately not wired, marked "unsafe?". That is why
a module cannot read any property value, only receive changes.

## What can be improved, and what it costs

Ordered by what is actually achievable on this body.

### 1. Close the loop in ML instead of trusting Canon (works today)

Canon's LiveView AF commits to a direction early. ML can do better with the same
hardware by *measuring*: a coarse sweep over a range, score every position, then a
fine sweep and a parabolic fit. Slow (about 0.15-0.2 s per position) but it cannot
"hunt past" the peak, because it looks at the whole range before deciding.

Cost: a module (metric + sweep + logging). No firmware risk: it only writes
`0x80050013` through the normal property API and reads the LiveView buffer.

### 2. A better metric than Canon uses, for hard scenes

Canon's LiveView AF is contrast-based on the display image. For night scenes or
low-contrast subjects, a metric tuned for the scene wins: star-core sharpness at
night, edge energy in the centre for portraits, and for astro a Bahtinov mask turns
focus into a geometry problem far more precise than any sharpness score
(`docs/night-af-draft.md`).

Cost: same module as 1, plus metric variants.

### 3. Use `prop_get_value` to see what the camera thinks (risky, large payoff)

With `0xFF1304DC` wired, ML could read `PROP_LV_FOCUS_DONE` and
`PROP_LV_LENS_DRIVE_REMOTE` values instead of inferring progress from log lines.
That turns "step and hope" into proper closed-loop control, and would also answer
what Canon's own AF is doing at any moment.

Cost and risk: ML's own source marks it unsafe -- it may take locks or sleep, and a
bad call from the wrong task can hang the camera. This is an experiment for a body
that can be power-cycled, not a feature.

### 4. Viewfinder AF cannot be improved from ML at all

The phase-difference path needs the mirror down and the dedicated sensor, and the
measurement is not exposed. Nothing a module does changes it. The one ML-side lever
is AF micro-adjust (`src/afma.c`), which shifts the stored calibration.

### 5. Things that sound good and are not worth doing

* **Reading the metric out of firmware**: no string carries a focus evaluation
  value, three windows are excluded, and there is no lookup table. Weeks of work
  for something the camera does not expose.
* **Calling `NWCOM`-style internal entry points to drive AF**: the AF entry points
  are component callbacks that dereference their context; the camera's own property
  path is the supported one and already works from ML.

## The one measurement that would settle #1 and #3

Run `aftrace` (already in the build) for 30 s while focusing with both Canon's AF
and ML's focus steps. The log gives, for each change of `0x80050029`,
`0x80050027`, `0x80050002`, `0x80050013`, `0x80050008`, `0x80050010`, the value and
the **task name** that produced it. That names the task behind the algorithm, and
shows whether Canon's own AF and ML's steps use the same property path.
