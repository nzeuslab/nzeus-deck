# bar_chart.py - draws a simple bar chart with text characters.
temps = [("Sun", 41), ("Mon", 43), ("Tue", 45), ("Wed", 44), ("Thu", 46), ("Fri", 42), ("Sat", 40)]
print("A hot city, highest temperature (C)")
for day, t in temps:
    print("%s %s %d" % (day, "#" * (t - 30), t))
