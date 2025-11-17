import os
import json

with open('soderzhanie.txt', 'r') as f:
    tree = json.load(f)

source_exts = {'.cpp', '.c', '.h', '.hpp', '.cc', '.cxx', '.mm', '.m'}
exclude_dirs = {'build-debug', 'cmake-build-debug', '.git', '.idea',
                'CMakeFiles', '_deps', 'objects', 'pack', '.github'}

def collect(node, path=''):
    res = []
    if node['type'] == 'file':
        name = node['name']
        if os.path.splitext(name)[1] in source_exts:
            res.append(os.path.join(path, name))
    elif node['type'] == 'directory':
        name = node['name']
        if name in exclude_dirs:
            return res
        new_path = os.path.join(path, name) if path else name
        for c in node.get('children', []):
            res.extend(collect(c, new_path))
    return res

files = collect(tree)

with open('output.txt', 'w') as out:
    out.write(f"// {len(files)} files\n\n")
    for f in sorted(files):
        out.write(f"=== {f} ===\n")
        try:
            with open(f, 'r', encoding='utf-8', errors='ignore') as src:
                out.write(src.read())
        except:
            out.write("// not found\n")
        out.write("\n\n")

print(f"{len(files)} → output.txt")
