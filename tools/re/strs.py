import sys,re
d=open('text.bin','rb').read()
lines=open('text.asm').read().split('\n')
s,e=int(sys.argv[1],16),int(sys.argv[2],16)
for l in lines:
    m=re.match(r'\s+([0-9a-f]+):',l)
    if not m: continue
    a=int(m.group(1),16)
    if s<=a<e:
        mm=re.search(r'# 0x([0-9a-f]+)',l)
        extra=''
        if mm and 'lea' in l:
            t=int(mm.group(1),16)
            if t<len(d):
                z=d[t:t+80].split(b'\0')[0]
                if len(z)>3 and all(32<=c<127 for c in z): extra='  "%s"'%z.decode()
        if extra or 'call' in l: print(l.strip()[:90]+extra)
