/* probe.c - reads the NZEUS Probe's answers (see probe.h and
 * R36S/apps/probe/PROTOCOL.md). Everything is checked: a damaged or
 * unexpected line is refused rather than shown as a wrong measurement. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

int probe_take_line(ProbeLines *pl, const char *data, int n, int *used, char *out, size_t out_n)
{
    int i = 0;
    for (; i < n; i++) {
        char c = data[i];
        if (c == '\r') continue;                     /* (Arduino's println ends with CR LF) */
        if (c == '\n') {
            if (!pl->len) continue;                  /* an empty line: nothing to report */
            size_t k = (size_t)pl->len < out_n - 1 ? (size_t)pl->len : out_n - 1;
            memcpy(out, pl->buf, k);
            out[k] = 0;
            pl->len = 0;
            *used = i + 1;
            return 1;
        }
        if (pl->len < PROBE_LINE_MAX - 1) pl->buf[pl->len++] = c;   /* (too long: the rest is dropped) */
    }
    *used = i;
    return 0;
}

/* one whole number at *p (after spaces), moves *p past it; 1 = ok */
static int num(const char **p, long *v, int base)
{
    char *end;
    while (**p == ' ') (*p)++;
    if (!**p || **p == '-' || **p == '+') return 0;
    long x = strtol(*p, &end, base);
    if (end == *p || (*end && *end != ' ')) return 0;
    *p = end;
    *v = x;
    return 1;
}
static int at_end(const char *p) { while (*p == ' ') p++; return !*p; }

int probe_parse_id(const char *line, ProbeInfo *info)
{
    long ver;
    const char *p = line + 8;
    if (strncmp(line, "NZPROBE ", 8) || !num(&p, &ver, 10) || ver != 1) return 0;
    while (*p == ' ') p++;
    if (!*p || strlen(p) >= sizeof info->board) return 0;
    snprintf(info->board, sizeof info->board, "%s", p);
    return 1;
}

int probe_parse_info(const char *line, ProbeInfo *info)
{
    int adc, logic, vref, pwm;
    if (sscanf(line, "INFO adc=%d logic=%d vref=%d pwm=%d", &adc, &logic, &vref, &pwm) != 4) return 0;
    if (adc < 0 || adc > 8 || logic < 0 || logic > 16 || vref < 1000 || vref > 6000) return 0;
    info->adc = adc;
    info->logic = logic;
    info->vref = vref;
    return 1;
}

int probe_parse_volt(const char *line, int *ch, int *mv)
{
    long c, v;
    const char *p = line + 1;
    if (line[0] != 'V' || !num(&p, &c, 10) || !num(&p, &v, 10) || !at_end(p)) return 0;
    if (c < 1 || c > 8 || v > 6000) return 0;
    *ch = (int)c;
    *mv = (int)v;
    return 1;
}

int probe_parse_logic(const char *line, int *levels, int max)
{
    int n = 0;
    if (strncmp(line, "L ", 2)) return 0;
    for (const char *p = line + 2; *p; p++) {
        if (*p != '0' && *p != '1') return 0;
        if (n < max) levels[n] = *p == '1';
        n++;
    }
    return n <= max ? n : 0;
}

int probe_parse_scope(const char *line, int *v, int max, int *us)
{
    long n, t, x;
    const char *p = line + 1;
    if (line[0] != 'S' || !num(&p, &n, 10) || !num(&p, &t, 10)) return 0;
    if (n < 2 || n > max || n > PROBE_MAX_SAMPLES) return 0;
    for (long i = 0; i < n; i++) {
        if (!num(&p, &x, 10) || x > 6000) return 0;
        v[i] = (int)x;
    }
    if (!at_end(p)) return 0;                        /* more values than it said */
    *us = (int)t;
    return (int)n;
}

int probe_parse_i2c(const char *line, int *addrs, int max)
{
    long a;
    int n = 0;
    const char *p = line + 1;
    if (!strcmp(line, "I none")) return 0;
    if (line[0] != 'I' || line[1] != ' ') return -1;
    while (!at_end(p)) {
        if (!num(&p, &a, 16) || a < 0x08 || a > 0x77) return -1;
        if (n < max) addrs[n++] = (int)a;
    }
    return n ? n : -1;
}

/* well-known chips, by I2C address (many share one: this is a hint, not proof) */
static const struct { int addr; const char *name; } chips[] = {
    { 0x1E, "HMC5883L compass" },
    { 0x20, "PCF8574 / MCP23017 pin expander" },
    { 0x23, "BH1750 light sensor" },
    { 0x27, "LCD backpack (PCF8574)" },
    { 0x29, "VL53L0X distance / TCS34725 colour" },
    { 0x38, "AHT10/AHT20 temperature + humidity" },
    { 0x39, "TSL2561 light / APDS9960 gesture" },
    { 0x3C, "OLED screen (SSD1306)" },
    { 0x3D, "OLED screen (SSD1306)" },
    { 0x40, "INA219 power meter / HDC1080" },
    { 0x41, "INA219 power meter" },
    { 0x44, "SHT3x temperature + humidity" },
    { 0x48, "ADS1115 ADC / TMP102 temperature" },
    { 0x50, "EEPROM (24Cxx)" },
    { 0x53, "ADXL345 accelerometer" },
    { 0x57, "EEPROM on a clock module (AT24C32)" },
    { 0x5A, "MLX90614 IR thermometer / CCS811 air" },
    { 0x60, "Si5351 clock generator / MCP4725 DAC" },
    { 0x68, "DS3231 clock / MPU6050 motion" },
    { 0x69, "MPU6050 motion (AD0 high)" },
    { 0x76, "BME280 / BMP280 pressure" },
    { 0x77, "BME280 / BMP180 pressure" },
};

const char *probe_i2c_name(int addr)
{
    for (size_t i = 0; i < sizeof chips / sizeof chips[0]; i++)
        if (chips[i].addr == addr) return chips[i].name;
    return NULL;
}
