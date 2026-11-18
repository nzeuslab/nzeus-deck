# ohms_law.py - works out voltage, current or resistance (V = I x R).
print("Ohm's law: V = I x R")
print("Leave the one you want to find empty.")
v = input("Voltage in volts (V): ").strip()
i = input("Current in amps (A): ").strip()
r = input("Resistance in ohms: ").strip()
if v == "":
    print("V = %.3f volts" % (float(i) * float(r)))
elif i == "":
    print("I = %.4f amps (%.1f mA)" % (float(v) / float(r), 1000 * float(v) / float(r)))
elif r == "":
    print("R = %.1f ohms" % (float(v) / float(i)))
else:
    print("Leave one of them empty, and I will work it out.")
