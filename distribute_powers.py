#!/usr/bin/env python3
import random

POWERS = [
    "BunnyHop",
    "Only Headshot",
    "Wallhack",
    "Player model size",
    "Invisibility",
    "Miss damage",
    "Speed",
    "No recoil",
    "Infinite ammo",
    "HP management",
    "Knife only",
    "Broken shot registration",
    "Teleport",
]


def get_names():
    print("Enter the names of people (one per line). Press Enter on an empty line to finish.")
    names = []
    while True:
        try:
            name = input(f"  {len(names) + 1}. ").strip()
        except EOFError:
            break
        if not name:
            break
        names.append(name)
    return names


def distribute(names):
    pool = POWERS.copy()
    random.shuffle(pool)
    assignments = {}
    for i, name in enumerate(names):
        assignments[name] = pool[i % len(pool)]
        if (i + 1) % len(POWERS) == 0:
            random.shuffle(pool)
    return assignments


def main():
    print("=" * 40)
    print("  Power Distribution")
    print("=" * 40)
    names = get_names()
    if not names:
        print("\nNo names entered. Exiting.")
        return
    assignments = distribute(names)
    print("\n" + "=" * 40)
    print("  Results")
    print("=" * 40)
    width = max(len(n) for n in names)
    for name, power in assignments.items():
        print(f"  {name.ljust(width)}  ->  {power}")
    print("=" * 40)


if __name__ == "__main__":
    main()
