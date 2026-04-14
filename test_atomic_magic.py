with open("mybuddy.h", "r") as f:
    lines = f.readlines()
for i, line in enumerate(lines):
    if "magic" in line and "_Atomic" in line:
        print(f"{i+1}: {line.strip()}")
