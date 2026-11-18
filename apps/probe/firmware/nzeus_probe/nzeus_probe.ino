/* nzeus_probe.ino - the NZEUS Probe: a Raspberry Pi Pico or STM32 board on
 * USB that measures things for the NZEUS console (voltmeter, logic probe,
 * simple scope, signal generator, I2C scanner).
 *
 * The console sends one line ("V 1"), the board answers one line ("V 1 1650").
 * All commands: ../../PROTOCOL.md. You can also type them yourself in the
 * Arduino IDE's Serial Monitor (115200, "Newline").
 *
 * Upload with the Arduino IDE:
 *   Pico:  board package "Raspberry Pi Pico/RP2040" (Earle Philhower)
 *   STM32: board package "STM32 MCU based boards" (STM32duino), USB serial on
 *          (Tools > USB support > CDC), or use the Nucleo's ST-Link serial port
 *
 * INPUTS TAKE 0 TO 3.3 V ONLY. Higher voltages need a resistor divider. */
#include <Wire.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the pins, per board ---------- */
#if defined(ARDUINO_ARCH_RP2040)
#define BOARD "pico"
static const int ADC_PINS[] = { 26, 27 };            /* GP26, GP27 */
static const int LOGIC_PINS[] = { 10, 11, 12, 13 };  /* GP10..GP13 (GP4/GP5 are I2C) */
#define PWM_PIN 15                                   /* GP15 */
#define ADC_BITS 12
#define VREF_MV 3300
#elif defined(ARDUINO_ARCH_STM32)
#define BOARD "stm32"
static const int ADC_PINS[] = { A0, A1 };
static const int LOGIC_PINS[] = { 2, 3, 4, 5 };      /* D2..D5 */
#define PWM_PIN 9                                    /* D9 */
#define ADC_BITS 12
#define VREF_MV 3300
#else                                                /* any other Arduino (5 V ones too) */
#define BOARD "arduino"
static const int ADC_PINS[] = { A0, A1 };
static const int LOGIC_PINS[] = { 2, 3, 4, 5 };
#define PWM_PIN 9
#define ADC_BITS 10
#define VREF_MV 5000
#endif

#define N_ADC   (int)(sizeof ADC_PINS / sizeof ADC_PINS[0])
#define N_LOGIC (int)(sizeof LOGIC_PINS / sizeof LOGIC_PINS[0])
#define MAX_SAMPLES 200
#define LINE_MAX 40
#define REPLY_MAX 1400            /* "S 200 100 " + 200 values of up to 5 digits */

/* ---------- small helpers ---------- */

/* reads one whole number from *p (after spaces); 1 = ok. Anything else
 * (letters, a missing number, a minus sign) = 0, so bad commands are refused. */
static int read_num(const char **p, long *v)
{
    const char *s = *p;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return 0;
    long n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        if (n > 100000000L) return 0;         /* far too big for anything here */
        s++;
    }
    if (*s != ' ' && *s != 0) return 0;       /* "12abc" */
    *p = s;
    *v = n;
    return 1;
}
/* nothing but spaces left? */
static int at_end(const char *p)
{
    while (*p == ' ') p++;
    return *p == 0;
}

/* an analog pin's voltage in millivolts */
static long read_mv(int ch)
{
    long raw = analogRead(ADC_PINS[ch - 1]);
    return raw * VREF_MV / ((1L << ADC_BITS) - 1);
}

/* the signal generator: hz = 0 switches it off */
static void set_pwm(long hz, long duty)
{
#if defined(ARDUINO_ARCH_RP2040)
    if (hz == 0) { analogWrite(PWM_PIN, 0); return; }
    analogWriteFreq(hz);
    analogWriteRange(100);                    /* so the duty is simply 0..100 */
    analogWrite(PWM_PIN, duty);
#elif defined(ARDUINO_ARCH_STM32)
    if (hz == 0) { analogWrite(PWM_PIN, 0); return; }
    analogWriteFrequency(hz);
    analogWrite(PWM_PIN, duty * 255 / 100);   /* (8-bit by default) */
#else
    (void)duty;                               /* a plain Arduino: square wave, 50 % only */
    if (hz == 0) { noTone(PWM_PIN); return; }
    tone(PWM_PIN, hz);
#endif
}

/* ---------- the commands ---------- */

/* handles one command line and writes the one-line answer into reply */
void probe_command(const char *line, char *reply, size_t n)
{
    const char *p = line + 1;                 /* (after the command letter) */
    long a, b, c;

    if (!strcmp(line, "ID")) {
        snprintf(reply, n, "NZPROBE 1 %s", BOARD);
    } else if (!strcmp(line, "INFO")) {
        snprintf(reply, n, "INFO adc=%d logic=%d vref=%d pwm=1", N_ADC, N_LOGIC, VREF_MV);
    } else if (line[0] == 'V' && (line[1] == ' ' || !line[1])) {          /* voltmeter */
        if (!read_num(&p, &a) || !at_end(p)) snprintf(reply, n, "ERR usage: V <channel>");
        else if (a < 1 || a > N_ADC) snprintf(reply, n, "ERR channel 1..%d", N_ADC);
        else snprintf(reply, n, "V %ld %ld", a, read_mv((int)a));
    } else if (!strcmp(line, "L")) {                                       /* logic probe */
        int k = snprintf(reply, n, "L ");
        for (int i = 0; i < N_LOGIC && k < (int)n - 1; i++) reply[k++] = digitalRead(LOGIC_PINS[i]) ? '1' : '0';
        reply[k] = 0;
    } else if (line[0] == 'S' && (line[1] == ' ' || !line[1])) {          /* scope */
        if (!read_num(&p, &a) || !read_num(&p, &b) || !read_num(&p, &c) || !at_end(p))
            snprintf(reply, n, "ERR usage: S <channel> <samples> <us>");
        else if (a < 1 || a > N_ADC) snprintf(reply, n, "ERR channel 1..%d", N_ADC);
        else if (b < 2 || b > MAX_SAMPLES) snprintf(reply, n, "ERR samples 2..%d", MAX_SAMPLES);
        else if (c < 20 || c > 100000) snprintf(reply, n, "ERR us 20..100000");
        else {
            static int v[MAX_SAMPLES];
            unsigned long t0 = micros();
            for (long i = 0; i < b; i++) {
                while ((long)(micros() - (t0 + (unsigned long)(i * c))) < 0) { }   /* wait for its moment */
                v[i] = (int)read_mv((int)a);
            }
            int k = snprintf(reply, n, "S %ld %ld", b, c);
            for (long i = 0; i < b && k < (int)n - 8; i++) k += snprintf(reply + k, n - k, " %d", v[i]);
        }
    } else if (line[0] == 'P' && (line[1] == ' ' || !line[1])) {          /* signal generator */
        if (!read_num(&p, &a) || !read_num(&p, &b) || !at_end(p)) snprintf(reply, n, "ERR usage: P <hz> <duty>");
        else if (a > 100000) snprintf(reply, n, "ERR hz 0..100000");
        else if (b < 1 || b > 99) snprintf(reply, n, "ERR duty 1..99");
        else { set_pwm(a, b); snprintf(reply, n, "P %ld %ld", a, b); }
    } else if (!strcmp(line, "I")) {                                       /* I2C scanner */
        int k = snprintf(reply, n, "I");
        int found = 0;
        for (int addr = 0x08; addr <= 0x77; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) { k += snprintf(reply + k, n - k, " %02X", addr); found++; }
        }
        if (!found) snprintf(reply, n, "I none");
    } else {
        snprintf(reply, n, "ERR unknown command");
    }
}

/* ---------- Arduino start + main loop ---------- */
static char line[LINE_MAX + 1];
static int len, too_long;

void setup()
{
    Serial.begin(115200);
#if ADC_BITS == 12
    analogReadResolution(12);
#endif
    for (int i = 0; i < N_LOGIC; i++) pinMode(LOGIC_PINS[i], INPUT);
    pinMode(PWM_PIN, OUTPUT);
    Wire.begin();
}

void loop()
{
    static char reply[REPLY_MAX];
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {               /* one line is complete */
            if (too_long) Serial.println("ERR line too long");
            else if (len) {
                line[len] = 0;
                probe_command(line, reply, sizeof reply);
                Serial.println(reply);
            }
            len = 0;
            too_long = 0;
        } else if (len < LINE_MAX) {
            line[len++] = ch;
        } else {
            too_long = 1;                             /* (the rest of it is dropped) */
        }
    }
}
