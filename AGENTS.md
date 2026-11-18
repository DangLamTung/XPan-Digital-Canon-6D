# Rules for working on this repository

Written because a session shipped "fixed" claims that were not checked, twice,
and the user had to catch it.

## Before saying a change is done

Every claim in a reply must be backed by something in this session's tool output.
If a check was not run, say it was not run.

The checklist, in order, for anything that goes to the card:

1. **Build** -- `./build.sh 6D.116 clean`, and read the tail for `done:`.
2. **What is in the zip** -- `unzip -l .../magiclantern.zip` for the files that
   matter (a module removed from a directory is not removed from the zip; it is
   removed from `modules/Makefile`, `modules.included` and the stale
   `build/zip/` staging copy).
3. **Install** -- `./install-sd.sh /Volumes/EOS_DIGITAL`, then compare
   `md5 -q` of the card's `.mo` with the build's. If they differ, the card is not
   what was tested.
4. **What is actually on the card** -- list it. A file that "was removed"
   earlier can be back after the next install.
5. **Menu structure** -- dump the menu arrays (`awk '/menu_entry X\[\]/,/^};/'`)
   and show the tree, do not describe it from memory.
6. **Then** write the reply, quoting the numbers from 1-5.

## Things this repository has already got wrong

* mlwifi was deleted from the card and came back on the next install, because it
  was still in `modules/Makefile` and `modules.included`.
* "No grain in develop" was caused by `amp = grain * 62 / 100 * 2 / 7` flooring to
  zero for GRAIN <= 6 -- not by the user's stock choice.
* The debug console was blamed on mlwifi without checking
  `CONFIG_INT("module.console", ...)`, which is its own persistent switch.
* A capture-time encoder crop was described as "not needing the missing
  megabyte" when the three links that matter (which function, what is in r0,
  what `+0x3c`/`+0x5c` mean) are all unproven.

## Style

Do not present a hypothesis as a result. Do not present a probe as a feature.
If something is blocked by missing data, say exactly what data is missing and
what it would answer.
