#ifndef NZEUS_PROBE_H
#define NZEUS_PROBE_H
#include <stddef.h>

/* The NZEUS Probe: a Pico or STM32 board on USB that measures for the console
 * (voltmeter, logic probe, scope, signal generator, I2C scanner). It speaks
 * one line of text per command (R36S/apps/probe/PROTOCOL.md). This file reads
 * its answers; app_probe.c is the screen. */

#define PROBE_MAX_SAMPLES 200
#define PROBE_LINE_MAX    1500

typedef struct {
    char board[16];      /* "pico", "stm32", "arduino" */
    int  adc, logic, vref;
} ProbeInfo;

/* collects bytes from the serial port into whole lines */
typedef struct {
    char buf[PROBE_LINE_MAX];
    int  len;
} ProbeLines;

/* add what arrived; 1 = a whole line is ready in out (without the newline) */
int probe_take_line(ProbeLines *pl, const char *data, int n, int *used, char *out, size_t out_n);

/* the answers (1 = understood, 0 = not that answer / damaged) */
int probe_parse_id(const char *line, ProbeInfo *info);                  /* "NZPROBE 1 pico" */
int probe_parse_info(const char *line, ProbeInfo *info);                /* "INFO adc=2 logic=4 vref=3300 pwm=1" */
int probe_parse_volt(const char *line, int *ch, int *mv);               /* "V 1 1650" */
int probe_parse_logic(const char *line, int *levels, int max);          /* "L 1011" -> count of pins */
int probe_parse_scope(const char *line, int *v, int max, int *us);      /* "S n us v..." -> n */
int probe_parse_i2c(const char *line, int *addrs, int max);             /* "I 3C 68" -> count (0 = none), -1 = damaged */

/* a common chip at this I2C address (a guess: several chips share addresses), or NULL */
const char *probe_i2c_name(int addr);

#endif
