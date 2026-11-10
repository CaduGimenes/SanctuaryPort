import sys,re
lines=open('text.asm').read().split('\n')
idx={}
for i,l in enumerate(lines):
    m=re.match(r'\s+([0-9a-f]+):',l)
    if m: idx[int(m.group(1),16)]=i
addrs=sorted(idx)
import bisect
def start(a):
    i=idx[a]
    while i>0:
        if re.search(r'\tpush\s+%rbp$',lines[i]) and re.search(r'\tmov\s+%rsp,%rbp',lines[i+1]): 
            return int(re.match(r'\s+([0-9a-f]+):',lines[i]).group(1),16)
        i-=1
for a in sys.argv[1:]:
    s=start(int(a,16)); 
    callers=[l.split(':')[0].strip() for l in lines if re.search(r'call\s+0x%x$'%s,l)]
    print(a,'func',hex(s),'callers',callers[:8])
