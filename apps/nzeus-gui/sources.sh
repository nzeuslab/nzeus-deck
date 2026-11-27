# The list of NZEUS GUI source files, shared by build_nzeus_modern.sh (the
# console program), build_nzeus_gui.sh (the PC tests) and
# test_nzeus_gui_mutations.sh (add every new .c file here).
# UI_FILES:      the screens and built-in apps (the PC preview uses only these)
# CONSOLE_FILES: the console glue around them (libretro core, USB mouse/keyboard)
# hw.c + hw_modern.c: the console's hardware on the SD Debian (Linux 7.2)
UI_FILES="theme.c settings.c icons.c font_mono24.c hw.c hw_modern.c app_settings.c nzlist.c app_python.c picture.c app_images.c bgjob.c wifi.c app_wifi.c audio.c app_media.c serial.c app_arduino.c obd.c app_car.c shot.c sha256.c keyvault.c vtwidget.c app_claude.c app_electronics.c elec_calc.c app_web.c procs.c app_taskmgr.c calc_eval.c app_calculator.c app_notes.c app_clock.c home.c display.c ui.c desktop.c sysinfo.c apps.c font_mono16.c
          nzui.c fsops.c app_terminal.c terminal.c term_cmds.c process.c app_files.c
          app_editor.c editor.c highlight.c"
# the Car app: the any-car scanner (carscan.c, Ford names in fordscan.c) + the PDF report maker.
# (carscan_sample.c, the sample scan, is NOT here: only the report's PC test uses it -
#  build_carreport_test.sh compiles it itself - so it never reaches the console.)
UI_FILES="$UI_FILES dtc_db.c carsum.c carscan.c fordscan.c carlog.c carreport.c carreport_text.c carreport_pdf.c font_report.c"
# Bluetooth keyboards and mice (the Settings app's Bluetooth section)
UI_FILES="$UI_FILES bt.c"
CONSOLE_FILES="core.c usbmouse.c usbkbd.c"
# other people's code, used as it is (compiled without our warning checks)
THIRD_FILES="vterm/src/encoding.c vterm/src/keyboard.c vterm/src/mouse.c vterm/src/parser.c
             vterm/src/pen.c vterm/src/screen.c vterm/src/state.c vterm/src/unicode.c vterm/src/vterm.c"
# libmad: the MP3 decoder (whole-number maths, see libmad/SOURCE.txt)
THIRD_FILES="$THIRD_FILES libmad/bit.c libmad/fixed.c libmad/frame.c libmad/huffman.c libmad/layer12.c
             libmad/layer3.c libmad/stream.c libmad/synth.c libmad/timer.c"
# the USB-C port: Devices / PC connection (USB network + serial), Settings > USB
UI_FILES="$UI_FILES usbmode.c"
# Arabic text on labels (WhatsApp, list rows) and the WhatsApp app (talks to the PC's bridge)
UI_FILES="$UI_FILES artext.c app_whatsapp.c"
# emoji pictures inside text (artext.c puts them in the font chain)
UI_FILES="$UI_FILES emoji.c"
# the NZEUS Probe: a Pico/STM32 measuring board on USB (probe.c reads its answers, app_probe.c is the screen)
UI_FILES="$UI_FILES probe.c app_probe.c"
# the Dev toolbox (Tools list): devtools.c does the work, app_devtools.c is the screen
UI_FILES="$UI_FILES devtools.c app_devtools.c"
# the Health dashboard (Tools list): health.c reads + judges, app_health.c is the screen
UI_FILES="$UI_FILES health.c app_health.c"
# Wi-Fi ID cards (the deck): security in words, same router / twins, best channel, maker
UI_FILES="$UI_FILES wifiinfo.c"
# Bluetooth ID cards (the deck): reads `nzeus-bt survey` lines, says what each device is
UI_FILES="$UI_FILES btinfo.c app_btscan.c"
# Update NZEUS over Wi-Fi (Tools list): the screen; the work is core/deck-modern/nzeus-update.sh
UI_FILES="$UI_FILES app_update.c"
# Files' PDF reader: Poppler renders one bounded page outside the GUI.
UI_FILES="$UI_FILES pdfreader.c"
UI_FILES="$UI_FILES carsupport.c"
# the desktop's picture wallpapers (Hills, the user's own Picture): drawn once into a buffer
UI_FILES="$UI_FILES wallpaper.c wallpaper_hill_data.c"   # (+ the built-in Hills picture, art/hill.jpg)
# the owner's own start-button logo and desktop icons (nzeus/start-icon.bmp, nzeus/icons/NAME.bmp)
UI_FILES="$UI_FILES ownicons.c retroicons_data.c"   # (+ Retro Blue's built-in app icons, art/retro-icons.png)
