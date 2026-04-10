import sys

def replace_block(filepath, search, replace):
    with open(filepath, 'r') as f:
        content = f.read()
    if search not in content:
        print(f"Error: Search block not found in {filepath}")
        return False
    content = content.replace(search, replace)
    with open(filepath, 'w') as f:
        f.write(content)
    return True

if __name__ == '__main__':
    with open(sys.argv[1], 'r') as f:
        search = f.read()
    with open(sys.argv[2], 'r') as f:
        replace = f.read()
    if replace_block(sys.argv[3], search, replace):
        print("Replaced successfully")
    else:
        sys.exit(1)
