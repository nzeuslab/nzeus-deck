# sample.py - shows every colour of the editor
import random


@staticmethod
def greet(name, times=3):
    """Say hello a few times.
    This text is a multi-line string."""
    for i in range(times):
        if i % 2 == 0:
            print(f"Hello {name}!", i)
        else:
            print('again', [1, (2, {3: 4})])
    return True


class Robot(object):
    def __init__(self):
        self.power = 0xFF  # full battery
        raise ValueError("not ready")
