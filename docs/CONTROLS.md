# Confirm and back buttons

Settings > Controls > Confirm button offers **A to accept** (the default) or
**B to accept**. Left/Right changes the choice; pressing the current confirm
button changes it too. The other button becomes Back/Cancel. The choice is
saved immediately as `accept_button=a` or `accept_button=b` in settings.ini.
Missing or unrecognized values use A.

The setting applies to the handheld UI, lists, pop-up menus, yes/no dialogs,
on-screen keyboards and the desktop pointer click. Button hints, including
combinations such as D-pad+A, update in already-open windows. USB keyboard
Enter/Esc and the mouse retain their usual behavior.

## UI code

`ui_key()` turns the physical A/B into logical accept/back once, before
routing; everything after it (apps, lists, pop-ups) and keyboard Enter/Esc use
logical keys. Hints follow the setting: `nz_hints()` and `nz_button_label()`
for keycaps, `{accept}`/`{back}` templates through `nz_button_text()` for
inline help (see `apps/nzeus-gui/nzui.h`).

Check it on the PC:

    NZ_CONTROLS_ONLY=1 bash build_nzeus_gui.sh pc tests
