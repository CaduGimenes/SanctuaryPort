import sys,re,subprocess
sys.path.insert(0, sys.argv[1])
from link_libc import module
m=module(sys.argv[2])
asm=open('text.asm').read()
names={}
for a in sys.argv[3:]:
    mm=re.search(r'^\s+%s:\s.*jmp\s+\*0x([0-9a-f]+)\(%%rip\)\s+# 0x([0-9a-f]+)'%a, asm, re.M)
    if not mm: print(a,'no jmp'); continue
    got=int(mm.group(2),16)
    for (target,typ,sym,add) in m['relocs']:
        if target==got:
            s=m['symbols'][sym]; print(a,'GOT',hex(got),s['identity'] or s['name'])
