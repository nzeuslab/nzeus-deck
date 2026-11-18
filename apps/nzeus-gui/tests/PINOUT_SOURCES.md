# Electronics app: where the pinouts and reference data were checked

Checked 2026-10-04 against the sources below. Data lives in `elec_calc.c` (boards,
colour code, E series, AWG, 555) and `app_electronics.c` (LED voltages, tolerance list).
Every fact marked "test" is checked by `tests/test_electronics.c`.

## STM32 "Blue Pill" (STM32F103C8T6)

Sources
- ST STM32F103x8/xB datasheet, pin definitions table (I/O level "FT" = 5 V tolerant),
  current characteristics, output driving current:
  https://www.st.com/resource/en/datasheet/stm32f103c8.pdf
  (st.com timed out; the same ST document was read from the copy at
  https://files.seeedstudio.com/wiki/Grove-Mini_Track_Ball/res/STM32F03C8T6.pdf)
- PC13-PC15 footnote of the current datasheet ("sinks a limited amount of current (3 mA) ...
  must not be used as a current source"), quoted e.g. in https://www.mikrocontroller.net/topic/427238
- stm32-base.org board page (header lists, SWD header, LED, 5 V warning) and its top-view photo:
  https://stm32-base.org/boards/STM32F103C8T6-Blue-Pill.html
  https://stm32-base.org/assets/img/boards/STM32F103C8T6_Blue_Pill-2.jpg

| Checked | Result | Change |
|---|---|---|
| 5 V tolerant pins | Datasheet: FT on PA8-PA15, PB2-PB4, PB6-PB15; NOT on PA0-PA7, PB0, PB1, PB5, PC13-PC15. App was right. | Test now also checks exactly 18 header pins carry `*` |
| Which pins face each other (orientation) | **Wrong.** Photo: USB end has G, G, 3.3, R on one side and B12 on the other; VB sits opposite the 3.3 at the SWD end. App had VB next to B12 at the USB end (one side was upside down). | USB up: left = B12 ... B9, 5V, G, 3.3; right = G, G, 3.3, R, B11 ... A0, C15, C14, C13, VB (test: exact order both sides, USB top / SWD bottom) |
| Pin functions (USART1/2/3, SPI1/2, I2C1/2, USB D-/D+, JTAG, TIM PWM, ADC numbers, 32 kHz crystal on C14/C15) | Match the datasheet alternate-function columns | none |
| Current per pin | **Misleading.** "Max 25 mA per pin" is the absolute max rating; the datasheet's working figure is +-8 mA (20 mA sink with a relaxed low level). PC13-15: 3 mA, must not source current. | Note: "Up to 8 mA per pin (25 mA = absolute max). C13, C14, C15: 3 mA, only to GND." |
| LED on C13, LOW = on | stm32-base: LED on PC13 in sink mode | "green LED" -> "the board's LED" (colour varies) |
| 5V pin | stm32-base: wired straight to USB 5 V, no protection | Added that note |
| SWD header 3V3, DIO, CLK, GND; BOOT0 0 = run, 1 = loader on A9/A10 | Match | none |

## Arduino Uno R3

Sources
- Arduino full pinout (TOP VIEW, USB and jack at the top): https://docs.arduino.cc/resources/pinouts/A000066-full-pinout.pdf
- Arduino UNO R3 datasheet, JANALOG / JDIGITAL tables: https://docs.arduino.cc/resources/datasheets/A000066-datasheet.pdf
- ATmega328P absolute max 40 mA per I/O pin (Microchip datasheet, Absolute Maximum Ratings):
  https://onlinedocs.microchip.com/pr/GUID-80B1922D-872B-40C8-A8A5-0CBE009FD908-en-US-3/GUID-79648EF1-C832-41D2-9DDF-EE5730AB313B.html

| Checked | Result | Change |
|---|---|---|
| Left: NC, IOREF, RESET, 3.3V, 5V, GND, GND, VIN, gap, A0-A5 | Match (JANALOG 1-14) | test: exact order + gap |
| Right: SCL, SDA, AREF, GND, D13-D8, gap, D7-D0 | Match (JDIGITAL 18-1) | test: exact order + gap |
| PWM ~ on 3, 5, 6, 9, 10, 11 | Match | test: exactly 6 |
| SPI D10-D13, INT0/1 on D2/D3, LED on D13, SDA/SCL = A4/A5 | Match | none |
| 20 mA per I/O, 3.3 V pin 50 mA, VIN 7-12 V recommended (6-20 V limit) | Match | none |

## Arduino Nano (ATmega328P)

Source: Arduino full pinout (USB at the top, ICSP at the bottom): https://docs.arduino.cc/resources/pinouts/A000005-full-pinout.pdf

| Checked | Result | Change |
|---|---|---|
| Left: D13, 3V3, AREF, A0-A7, 5V, RST, GND, VIN | Match | test: exact order |
| Right: D12-D2, GND, RST, D0 (RX), D1 (TX) | Match | test: exact order |
| PWM pins, A6/A7 analog only, 20 mA per pin, 3V3 50 mA, VIN 7-12 V | Match | none |

## ESP32 DevKit (30-pin DOIT DevKit V1, ESP32-WROOM-32)

Sources
- 30-pin DOIT DevKit V1 pinout (antenna up, USB down): https://randomnerdtutorials.com/wp-content/uploads/2018/08/ESP32-DOIT-DEVKIT-V1-Board-Pinout-30-GPIOs-Copy.png
  (from https://randomnerdtutorials.com/esp32-pinout-reference-gpios/)
- Espressif ESP-IDF GPIO notes (strapping pins 0, 2, 5, 12, 15; GPIO6-11 flash; GPIO34-39 input only,
  no pull-ups; ADC2 not usable with Wi-Fi): https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html
- ESP32 datasheet v5.3 (strapping defaults, MTDI = GPIO12 selects flash voltage, VIH max = VDD + 0.3 V,
  so not 5 V tolerant): https://documentation.espressif.com/esp32_datasheet_en.pdf

| Checked | Result | Change |
|---|---|---|
| Left: EN, VP, VN, D34, D35, D32, D33, D25, D26, D27, D14, D12, D13, GND, VIN | Match | test: exact order |
| Right: D23, D22, TX0, RX0, D21, D19, D18, D5, TX2 (IO17), RX2 (IO16), D4, D2, D15, GND, 3V3 | Match | test: exact order |
| Input only: VP (36), VN (39), 34, 35 | Match | test: these 4 and no other pin say "only" |
| Strapping pins: D12 LOW at boot, D2/D5/D15 boot pins (GPIO0 = BOOT button, not on the header) | Match | test: D2, D5, D12, D15 say "boot" |
| ADC1 = 32-39, ADC2 + touch on 4, 2, 15, 13, 12, 14, 27, 25, 26; DAC on 25/26 | Match | none |
| VSPI 23/19/18/5, I2C 21/22, UART2 16/17, LED on GPIO2 | Match | none |
| Flash pins 6-11 | Not on the 30-pin header, so not shown | none |

## Reference data

| Checked | Source | Result | Change |
|---|---|---|---|
| Resistor colours: digits, multipliers (gold x0.1, silver x0.01), tolerances | IEC 60062:2016 table on https://en.wikipedia.org/wiki/Electronic_color_code | **Incomplete.** Orange 0.05 %, yellow 0.02 %, grey 0.01 % are tolerance colours in IEC 60062:2016; the app rejected them (and a test said "orange is not a tolerance colour"). | Added to `ec_tolerance()` and the Tolerance list (11 colours); test now uses black/white as the invalid ones and checks the 3 new ones |
| Band range message | 4 bands: 0.10 ohm .. 99 GOhm; 5 bands: 1.00 ohm .. 999 GOhm | Said "0.1 Ohm to about 99 GOhm" for both | Message now per band count |
| E12 / E24 values | https://en.wikipedia.org/wiki/E_series_of_preferred_numbers | Match | none |
| 555: tH = 0.693 (R1+R2) C, tL = 0.693 R2 C, f = 1.44 / ((R1+2 R2) C), one pulse 1.1 R C, pin numbers | TI NE555 datasheet SLFS022K: https://www.ti.com/lit/ds/symlink/ne555.pdf | Match | none |
| AWG: d = 0.127 mm x 92^((36-n)/39); AWG 22 = 0.644 mm, 24 = 0.511, 10 = 2.588 | https://en.wikipedia.org/wiki/American_wire_gauge | Match | none |
| LED voltages: red 2.0, orange 2.0, yellow 2.1, green 2.2 / 3.0, blue 3.2, white 3.2, IR 1.2 V | Ranges on https://en.wikipedia.org/wiki/Light-emitting_diode_physics | Typical values inside the ranges (orange 2.0 is just under 2.03; a lower guess gives a bigger, safer resistor) | none |
