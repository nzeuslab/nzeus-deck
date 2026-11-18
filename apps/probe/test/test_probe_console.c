/* test_probe_console.c - PC test for the console's side of the NZEUS Probe
 * (apps/nzeus-gui/probe.c): reading the board's answers. */
#include <stdio.h>
#include <string.h>
#include "probe.h"

static int pass, fail;
static void check(int ok, const char *what)
{
    if (ok) pass++; else fail++;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

int main(void)
{
    ProbeInfo in = { "", 0, 0, 0 };
    check(probe_parse_id("NZPROBE 1 pico", &in) && !strcmp(in.board, "pico"), "ID: the board's name");
    check(!probe_parse_id("NZPROBE 2 pico", &in) && !probe_parse_id("HELLO", &in) && !probe_parse_id("NZPROBE 1 ", &in),
          "ID: another protocol version / not a probe / no name = refused");
    check(probe_parse_info("INFO adc=2 logic=4 vref=3300 pwm=1", &in) && in.adc == 2 && in.logic == 4 && in.vref == 3300,
          "INFO: channels, logic pins, vref");
    check(!probe_parse_info("INFO adc=99 logic=4 vref=3300 pwm=1", &in), "INFO: impossible numbers refused");

    int ch = 0, mv = 0;
    check(probe_parse_volt("V 1 1650", &ch, &mv) && ch == 1 && mv == 1650, "V: channel + millivolts");
    check(!probe_parse_volt("V 1", &ch, &mv) && !probe_parse_volt("V 1 16x0", &ch, &mv) &&
          !probe_parse_volt("V 1 -5", &ch, &mv) && !probe_parse_volt("V 1 1650 7", &ch, &mv) &&
          !probe_parse_volt("ERR channel 1..2", &ch, &mv), "V: missing / damaged / negative / extra / an error = refused");

    int lv[8];
    check(probe_parse_logic("L 1011", lv, 8) == 4 && lv[0] == 1 && lv[1] == 0 && lv[3] == 1, "L: four pins, pin 1 first");
    check(probe_parse_logic("L 10a1", lv, 8) == 0 && probe_parse_logic("L 101010101", lv, 8) == 0, "L: bad digits / too many pins refused");

    int v[PROBE_MAX_SAMPLES], us = 0;
    check(probe_parse_scope("S 4 100 10 20 30 40", v, PROBE_MAX_SAMPLES, &us) == 4 && us == 100 && v[0] == 10 && v[3] == 40,
          "S: the samples and the time between them");
    check(probe_parse_scope("S 4 100 10 20 30", v, PROBE_MAX_SAMPLES, &us) == 0 &&
          probe_parse_scope("S 3 100 10 20 30 40", v, PROBE_MAX_SAMPLES, &us) == 0 &&
          probe_parse_scope("S 4 100 10 20 3x 40", v, PROBE_MAX_SAMPLES, &us) == 0,
          "S: fewer / more values than it says, or a damaged one = refused (no half pictures)");

    int ad[16];
    check(probe_parse_i2c("I 3C 68", ad, 16) == 2 && ad[0] == 0x3C && ad[1] == 0x68, "I: addresses in hex");
    check(probe_parse_i2c("I none", ad, 16) == 0, "I none: nothing found");
    check(probe_parse_i2c("I 3C ZZ", ad, 16) == -1 && probe_parse_i2c("I 99", ad, 16) == -1 && probe_parse_i2c("I", ad, 16) == -1,
          "I: damaged / impossible address = refused");
    check(probe_i2c_name(0x3C) && strstr(probe_i2c_name(0x3C), "OLED") && probe_i2c_name(0x0A) == NULL,
          "I2C names: 0x3C is an OLED screen; an unknown address has no name");

    /* whole lines out of bytes that arrive in pieces */
    ProbeLines pl = { "", 0 };
    char line[PROBE_LINE_MAX];
    int used = 0;
    int got = probe_take_line(&pl, "V 1 16", 6, &used, line, sizeof line);
    check(!got && used == 6, "lines: half a line waits for the rest");
    got = probe_take_line(&pl, "50\r\nL 1011\r\n", 12, &used, line, sizeof line);
    check(got && !strcmp(line, "V 1 1650") && used == 4, "lines: CR LF ends it; the rest stays for the next call");
    got = probe_take_line(&pl, "50\r\nL 1011\r\n" + 4, 8, &used, line, sizeof line);
    check(got && !strcmp(line, "L 1011"), "lines: ...and the next line comes out next");

    printf("== %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
