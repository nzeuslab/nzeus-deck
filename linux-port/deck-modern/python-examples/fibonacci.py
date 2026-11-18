# fibonacci.py - each number is the sum of the two before it.
a, b = 0, 1
for n in range(1, 21):
    print("%2d: %d" % (n, a))
    a, b = b, a + b
