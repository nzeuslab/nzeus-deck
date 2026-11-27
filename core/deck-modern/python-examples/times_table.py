# times_table.py - prints a multiplication table for any number.
n = int(input("Which times table? "))
for i in range(1, 13):
    print("%2d x %d = %d" % (i, n, i * n))
