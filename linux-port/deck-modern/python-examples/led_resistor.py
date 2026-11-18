# led_resistor.py - which resistor does an LED need? R = (supply - LED voltage) / current
supply = float(input("Supply voltage (for example 5): "))
led = float(input("LED voltage (red 2.0, green 2.2, blue/white 3.0): "))
ma = float(input("LED current in mA (usually 10 to 20): "))
r = (supply - led) / (ma / 1000)
standard = [10, 12, 15, 18, 22, 27, 33, 39, 47, 56, 68, 82]   # E12 values
pick = None
for decade in (1, 10, 100, 1000, 10000):
    for s in standard:
        if s * decade >= r and pick is None:
            pick = s * decade
print("Exact value: %.0f ohms" % r)
print("Use the next bigger standard resistor: %d ohms" % pick)
print("It will use %.0f mW of power." % ((supply - led) * ma))
