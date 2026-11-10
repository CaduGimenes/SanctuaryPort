import os
import sys
sys.path.insert(0, sys.argv[1])
from link_libc import module
m=module(os.environ.get('D3_EBOOT', 'eboot.bin'))
want={int(a,16) for a in sys.argv[2:]}
for (target,typ,sym,add) in m['relocs']:
    if add in want: print('reloc at',hex(target),'->',hex(add),'type',typ)
