# guess.py - a tiny number-guessing game: the computer picks, you guess.
# Run it from the Python app, or type:  python guess.py
import random

secret = random.randint(1, 20)      # the computer picks a number from 1 to 20
tries = 0
print("I picked a number between 1 and 20. Can you guess it?")

while True:
    answer = input("Your guess: ")  # waits for you to type a number + Enter
    if not answer.strip().isdigit():
        print("Please type a whole number.")
        continue
    guess = int(answer)
    tries += 1
    if guess < secret:
        print("Too low!")
    elif guess > secret:
        print("Too high!")
    else:
        print("Correct! You needed", tries, "tries.")
        break
