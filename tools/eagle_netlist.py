#!/usr/bin/env python3
# Flat netlist from an Eagle XML schematic: one line per net, "NET: PART.PIN ...".
# Power/ground symbols are left out. Usage: eagle_netlist.py board.sch > board.netlist.txt
import sys, xml.etree.ElementTree as ET
from collections import defaultdict
f=sys.argv[1]
r=ET.parse(f).getroot()
parts={p.get("name"):(p.get("deviceset"),p.get("value") or "") for p in r.findall(".//parts/part")}
nets=defaultdict(list)
for n in r.findall(".//sheets/sheet/nets/net"):
    for pr in n.iter("pinref"):
        nets[n.get("name")].append((pr.get("part"),pr.get("pin")))
skip=lambda p: p.startswith(("#PWR","SUPPLY","GND"))
for name in sorted(nets):
    refs=[f"{p}.{pin}" for p,pin in nets[name] if not skip(p)]
    print(f"{name}: {' '.join(sorted(set(refs)))}")
