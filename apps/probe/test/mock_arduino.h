/* mock_arduino.h - a pretend Arduino for testing nzeus_probe.ino on the PC.
 * Pins, the I2C bus and the USB serial port are all fake: the test sets what
 * they "measure" and reads back what the sketch did. */
#pragma once
#include <stdio.h>
#include <string.h>
#include <string>
#include <set>

#define INPUT 0
#define OUTPUT 1
#define A0 54
#define A1 55

/* what the fake pins measure (the test sets these) */
extern int fake_adc[64];          /* analogRead() result per pin */
extern int fake_level[64];        /* digitalRead() result per pin */
extern unsigned long fake_us;     /* micros(): goes up a little at every call */
extern unsigned long fake_read_at[256];   /* micros() at each analogRead (the scope's timing) */
extern int fake_reads;
/* what the sketch did */
extern long fake_pwm_hz, fake_pwm_value, fake_tone_hz;

inline void pinMode(int, int) {}
inline int analogRead(int pin) { if (fake_reads < 256) fake_read_at[fake_reads++] = fake_us; return fake_adc[pin]; }
inline int digitalRead(int pin) { return fake_level[pin]; }
inline unsigned long micros() { return fake_us += 7; }
inline void analogReadResolution(int) {}
inline void analogWrite(int, long v) { fake_pwm_value = v; }
inline void analogWriteFreq(long hz) { fake_pwm_hz = hz; }        /* (Pico) */
inline void analogWriteRange(long) {}
inline void analogWriteFrequency(long hz) { fake_pwm_hz = hz; }   /* (STM32) */
inline void tone(int, long hz) { fake_tone_hz = hz; }
inline void noTone(int) { fake_tone_hz = 0; }

/* the fake I2C bus: the chips in fake_i2c answer */
extern std::set<int> fake_i2c;
struct FakeWire {
    int addr = 0;
    void begin() {}
    void beginTransmission(int a) { addr = a; }
    int endTransmission() { return fake_i2c.count(addr) ? 0 : 2; }   /* 0 = it answered */
};
extern FakeWire Wire;

/* the fake USB serial port: the test puts text in, reads the printed lines */
struct FakeSerial {
    std::string in, out;
    void begin(long) {}
    int available() { return (int)in.size(); }
    int read() { int c = (unsigned char)in[0]; in.erase(0, 1); return c; }
    void println(const char *s) { out += s; out += "\n"; }
};
extern FakeSerial Serial;
