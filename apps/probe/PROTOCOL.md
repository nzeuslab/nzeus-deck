# NZEUS Probe - how the console talks to the board

The NZEUS Probe is a small board (a Raspberry Pi Pico, or one of the STM32
boards) plugged into the console by USB. It does the fast measuring; the
console's Probe app shows the screens.

The board shows up on the console as a USB serial port (`/dev/ttyACM0`, the
`cdc-acm` driver the console already has). Speed 115200. Every message is one
line of plain text ending in a newline, so you can also try it from any serial
terminal (Arduino IDE Serial Monitor, PuTTY, the NZEUS Arduino app).

The console sends a command; the board answers with ONE line. A wrong command
gets `ERR <why>`. Numbers are plain decimal.

| Command | Meaning | Answer (example) |
|---|---|---|
| `ID` | who are you? | `NZPROBE 1 pico` (protocol version 1, board `pico` / `stm32` / `arduino`) |
| `INFO` | what can you measure? | `INFO adc=2 logic=4 vref=3300 pwm=1` |
| `V <ch>` | voltmeter: voltage on analog channel 1..adc | `V 1 1650` (millivolts) |
| `L` | logic probe: the logic pins, 1 = HIGH | `L 1011` (pin 1 first) |
| `S <ch> <n> <us>` | scope: n samples (2..200), one every us microseconds (20..100000) | `S 4 100 1650 1700 1720 1690` |
| `P <hz> <duty>` | signal generator on the PWM pin: hz (0 = off, max 100000), duty 1..99 % | `P 1000 50` |
| `I` | I2C scanner: which addresses answer | `I 3C 68` or `I none` |

Which pins (see the top of `firmware/nzeus_probe/nzeus_probe.ino`):

| | Pico | STM32 (Nucleo/Blue Pill) |
|---|---|---|
| Analog CH1, CH2 | GP26, GP27 | A0, A1 |
| Logic 1..4 | GP10, GP11, GP12, GP13 | D2, D3, D4, D5 |
| PWM out | GP15 | D9 |
| I2C SDA / SCL | GP4 / GP5 (Wire) | the board's default Wire pins |

**Safety:** the inputs take **0 to 3.3 V only**. Never connect them straight to
5 V, 12 V or mains. For higher voltages use a resistor divider, and always
connect the board's GND to the circuit's GND.
