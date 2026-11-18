/* test_probe.cpp - tests the NZEUS Probe sketch on the PC with a pretend
 * Arduino (mock_arduino.h). Built once per board type by test_probe.sh. */
#include "mock_arduino.h"
#include "../firmware/nzeus_probe/nzeus_probe.ino"

int fake_adc[64], fake_level[64], fake_reads;
unsigned long fake_us, fake_read_at[256];
long fake_pwm_hz, fake_pwm_value, fake_tone_hz;
std::set<int> fake_i2c;
FakeWire Wire;
FakeSerial Serial;

static int pass, fail;
static void check(bool ok, const char *what)
{
    if (ok) pass++; else fail++;
    printf("%s  [%s] %s\n", ok ? "PASS" : "FAIL", BOARD, what);
}
static std::string ask(const char *cmd)
{
    static char reply[REPLY_MAX];
    probe_command(cmd, reply, sizeof reply);
    return reply;
}

int main()
{
    setup();
    int a1 = ADC_PINS[0], top = (1 << ADC_BITS) - 1;
    check(ask("ID") == std::string("NZPROBE 1 ") + BOARD, "ID: protocol 1 + the board's name");
    check(ask("INFO").rfind("INFO adc=2 logic=4 vref=", 0) == 0, "INFO: 2 analog channels, 4 logic pins");

    fake_adc[a1] = top / 2;                      /* half of the range */
    long half = (long)(top / 2) * VREF_MV / top;
    check(ask("V 1") == "V 1 " + std::to_string(half), "V 1: half the range = half of vref in mV");
    fake_adc[a1] = top;
    check(ask("V 1") == "V 1 " + std::to_string(VREF_MV), "V 1: the top of the range = vref");
    check(ask("V 3").rfind("ERR channel", 0) == 0 && ask("V 0").rfind("ERR channel", 0) == 0, "V 3 / V 0: no such channel");
    check(ask("V").rfind("ERR usage", 0) == 0 && ask("V 1x").rfind("ERR usage", 0) == 0 &&
          ask("V -1").rfind("ERR usage", 0) == 0 && ask("V 1 2").rfind("ERR usage", 0) == 0,
          "V: a missing / bad / negative / extra number is refused");

    for (int i = 0; i < 4; i++) fake_level[LOGIC_PINS[i]] = (i != 1);
    check(ask("L") == "L 1011", "L: the logic pins as 1/0, pin 1 first");

    fake_adc[a1] = top; fake_reads = 0;
    std::string s = ask("S 1 4 100");
    std::string v = std::to_string(VREF_MV);
    check(s == "S 4 100 " + v + " " + v + " " + v + " " + v, "S 1 4 100: 4 samples");
    bool spaced = fake_reads == 4;
    for (int i = 1; i < fake_reads; i++)        /* (the pretend clock ticks in 7 us steps: one tick of slack) */
        spaced = spaced && fake_read_at[i] - fake_read_at[0] + 7 >= (unsigned long)(100 * i);
    check(spaced, "S: the samples are taken at least 'us' apart");
    check(ask("S 1 1 100").rfind("ERR samples", 0) == 0 && ask("S 1 201 100").rfind("ERR samples", 0) == 0,
          "S: fewer than 2 or more than 200 samples refused");
    check(ask("S 1 4 10").rfind("ERR us", 0) == 0 && ask("S 1 4").rfind("ERR usage", 0) == 0, "S: too fast / missing numbers refused");
    s = ask("S 2 200 100");
    int count = 0;
    for (char ch : s) count += ch == ' ';
    check(count == 202, "S 2 200 100: all 200 samples fit in one line");

    check(ask("P 1000 50") == "P 1000 50", "P 1000 50: answered");
#if defined(ARDUINO_ARCH_RP2040)
    check(fake_pwm_hz == 1000 && fake_pwm_value == 50, "P: Pico PWM at 1000 Hz, 50 %");
#elif defined(ARDUINO_ARCH_STM32)
    check(fake_pwm_hz == 1000 && fake_pwm_value == 127, "P: STM32 PWM at 1000 Hz, 50 % (of 255)");
#else
    check(fake_tone_hz == 1000, "P: a plain Arduino plays a 1000 Hz square wave");
#endif
    ask("P 0 50");
    check(fake_pwm_value == 0 || fake_tone_hz == 0, "P 0: the signal generator switches off");
    check(ask("P 200000 50").rfind("ERR hz", 0) == 0 && ask("P 1000 0").rfind("ERR duty", 0) == 0 &&
          ask("P 1000 100").rfind("ERR duty", 0) == 0, "P: too high / 0 % / 100 % refused");

    fake_i2c = { 0x3C, 0x68 };
    check(ask("I") == "I 3C 68", "I: the I2C chips that answer, in hex");
    fake_i2c.clear();
    check(ask("I") == "I none", "I: nothing on the bus says none");

    check(ask("X").rfind("ERR unknown", 0) == 0 && ask("VV 1").rfind("ERR unknown", 0) == 0 &&
          ask("id").rfind("ERR unknown", 0) == 0, "unknown commands get ERR");

    /* the main loop over the (fake) USB serial port */
    Serial.out.clear();
    Serial.in = "ID\r\n";
    loop();
    check(Serial.out == std::string("NZPROBE 1 ") + BOARD + "\n", "loop: one line in (CR LF) = one answer out");
    Serial.out.clear();
    Serial.in = std::string(60, 'A') + "\nINFO\n";
    loop();
    check(Serial.out.rfind("ERR line too long\nINFO ", 0) == 0, "loop: an over-long line is refused, the next one still works");

    printf("== %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
