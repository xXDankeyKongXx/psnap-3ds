"""Merges the manual_funcs of recomp/pokemonsnap.us.3ds.toml into the .main section of
recomp/pokemonsnap.syms.toml (in address order).

N64Recomp applies manual_funcs only to ELF input; with a symbols file it ignores them, so the
static callbacks they describe were never recompiled ("Failed to find function at 0x8002E2F8").
Safe to run again: functions already in the symbols file are skipped.
Usage: python -I tools/add_manual_funcs.py
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
config = (root / "recomp" / "pokemonsnap.us.3ds.toml").read_text()
syms_path = root / "recomp" / "pokemonsnap.syms.toml"
syms = syms_path.read_text()

manual = re.findall(r'\{ name = "(\w+)", section = "\.main", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', config)
start = syms.index('name = ".main"')
fstart = syms.index("functions = [\n", start) + len("functions = [\n")
fend = syms.index("\n]", fstart)
lines = syms[fstart:fend].split("\n")
entries = []
for line in lines:
    m = re.search(r"vram = (0x[0-9A-Fa-f]+)", line)
    entries.append((int(m.group(1), 16), line))
have = {v for v, _ in entries}
added = 0
for name, vram, size in manual:
    v = int(vram, 16)
    if v in have:
        continue
    entries.append((v, f'    {{ name = "{name}", vram = 0x{v:08X}, size = 0x{int(size, 16):X} }},'))
    added += 1
entries.sort()
syms = syms[:fstart] + "\n".join(line for _, line in entries) + syms[fend:]
syms_path.write_text(syms)
print(f"{len(manual)} manual functions, {added} added to .main")
