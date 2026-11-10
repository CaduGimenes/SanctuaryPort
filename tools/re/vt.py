import os
import sys
sys.path.insert(0, sys.argv[1])
from link_libc import module
m=module(os.environ.get('D3_EBOOT', 'eboot.bin'))
vt=int(sys.argv[2],16)
r={t:a for (t,typ,s,a) in m['relocs']}
for off in range(0,0x58,8): print(hex(off), hex(r.get(vt+off,0)))
