"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
The built-in file's lines carry signatures (Pattern/At) that find each site in any build. A Pattern
may list several forms of a site separated by '|' (all of them together must match exactly once); a
Line with Optional="true" is left out of a build that has not got its site (one that matches more
than once still leaves the whole patch out), and a patch whose other sites are all found is applied.

Code caves: a patch that adds code or data of its own declares a cave, <Cave Name="mystic_cave"
Size="79"/> next to its Targets, and writes it with Lines whose Target is the cave's name; {rel:NAME:N}
reaches it like any Target, and <Target Name="theme" Cave="seasons_cave" At="1200"/> names a place
inside it. compile_patches gives each cave of the selected patches its own 16-byte aligned block of
the zero padding that ends the code segment (allocate_caves; the patches in file order, each cave
once: patches that declare the same Name, all with the same Size, share it). Region="data" asks for
zero variables instead, in the unused rest of the last data page (no Line may write there). The
blocks depend only on the eboot and the selected patches, never on an Address; check_overlaps then
fails on two writes of the selected patches that overlap (a patch may only overwrite its Base's
bytes) and on a write that leaves its cave.

Value parameters: a patch that writes one value chosen at start (a frame rate, a render size, a
season) is one Metadata with Params="name ..." instead of one Metadata per value. In a bytes Line,
{param:NAME:TYPE} is replaced by the value given for NAME: TYPE u8/u16/u32/u64 or i8..i64 (an
integer, little-endian), f32/f64 (a float), N (N raw bytes, given as hex) or aN (exactly N ASCII
characters). A Line with If="NAME" is written only when NAME is 1/true (0/false leaves it out). The
caller gives the values: compile_patches(..., params={'frame_wait': '8341'}), --param NAME=VALUE.
A <Preset Name="old name" NAME="value" .../> in the Metadata stands for the patch with those values:
selecting "Offline Seasons - Season 30" selects "Offline Seasons - Season" with that season's values.
A value no patch of the file declares, or a declared one a selected patch lacks, is an error.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.02"), or None when it cannot be read."""
    return game_sfo(game).get('APP_VER')


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def cave_regions(elf):
    """Where caves may go in this eboot, as image offsets: {'code': (start, end), 'data': (start, end)}.
    code: the zero bytes that end the executable PT_LOAD (from the 16-byte boundary after its last
    byte that is not zero to the segment's end); data: from the end of the writable PT_LOAD's memory
    to the end of its host page, which the loader maps with it and leaves zero."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    regions={}
    for i in range(phnum):
        kind,flags,offset,vaddr,_,filesz,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind!=1: continue
        if flags&1 and 'code' not in regions:
            used=len(elf[offset:offset+filesz].rstrip(b'\0'))
            regions['code']=(min(vaddr+align(used),vaddr+filesz),vaddr+filesz)
        elif flags&2:
            regions['data']=(align(vaddr+memsz),align(vaddr+memsz,0x1000))
    return regions


def eboot_image(elf):
    """The PT_LOAD file bytes of eboot.elf at their virtual addresses (image offsets)."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    loads=[struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize) for i in range(phnum)]
    loads=[(vaddr,offset,filesz) for kind,_,offset,vaddr,_,filesz,_,_ in loads if kind==1]
    image=bytearray(max(vaddr+filesz for vaddr,_,filesz in loads))
    for vaddr,offset,filesz in loads: image[vaddr:vaddr+filesz]=elf[offset:offset+filesz]
    return bytes(image)


# Signatures find a patch site in any build whose code matches, wherever the code moved: the
# original bytes around the site, "??" where a build may differ (rip-relative displacements,
# rel32 branch targets). tools/make_signatures.py writes them from the reference build.
def parse_pattern(text):
    return [None if b=='??' else int(b,16) for b in text.split()]


def find_pattern(image, pattern, limit=2):
    """Image offsets where pattern matches, at most limit of them."""
    runs,start=[],None
    for i,b in enumerate(pattern+[None]):
        if b is not None and start is None: start=i
        if b is None and start is not None: runs.append((start,i)); start=None
    if not runs: raise ValueError('pattern without fixed bytes')
    a,b=max(runs,key=lambda r:r[1]-r[0])
    anchor=bytes(pattern[a:b])
    found,at=[],image.find(anchor)
    while at>=0 and len(found)<limit:
        s=at-a
        if s>=0 and s+len(pattern)<=len(image) and all(p is None or image[s+i]==p for i,p in enumerate(pattern)):
            found.append(s)
        at=image.find(anchor,at+1)
    return found


class SignatureNotFound(LookupError):
    """No match of a signature in this build (an Optional Line is then left out; ambiguity never is)."""


def locate(image, element, prefix=''):
    """Image offset of a Line's or Target's site: its Pattern plus At, else Address. A Pattern may
    be several forms of the site separated by '|' (builds that compiled it differently): together
    they must match exactly once."""
    pattern=element.get(prefix+'Pattern')
    if not pattern:
        return int(element.get('Address'),0)-EBOOT_BASE
    found=set()
    for form in pattern.split('|'):
        found.update(find_pattern(image,parse_pattern(form)))
        if len(found)>1:
            raise LookupError('signature found more than once')
    if not found:
        raise SignatureNotFound('signature not found')
    return found.pop()+int(element.get(prefix+'At','0'),0)


OMITTED=object()  # the site of an Optional Line that this build has not got


def locate_line(image, line):
    """locate() of a Line; OMITTED for an Optional="true" Line whose signature this build has not got
    (a signature found more than once still leaves the whole patch out)."""
    try:
        return locate(image,line)
    except SignatureNotFound:
        if line.get('Optional','false').lower()=='true': return OMITTED
        raise


def resolve_targets(image, meta, sites):
    """Image offsets of a patch's named Targets: a site (Pattern/At), or the address a rip-relative
    or rel32 operand points to (Ref: the operand's offset in its Pattern; Tail: the instruction's
    bytes after the operand)."""
    targets={}
    for target in meta.iter('Target'):
        name=target.get('Name')
        if target.get('Cave'): continue  # a place in a cave: cave_targets, once the caves are placed
        if target.get('Line'):
            targets[name]=sites[int(target.get('Line'))]
            if targets[name] is OMITTED:
                raise SignatureNotFound(f'signature not found (Target {name} is at an Optional Line left out)')
            continue
        at=locate(image,target)
        if target.get('Ref'):
            disp,=struct.unpack_from('<i',image,at)
            at=at+4+int(target.get('Tail','0'))+disp
        targets[name]=at
    return targets


def fill(value, at, targets):
    """Value bytes with each {rel:NAME:TAIL} replaced by the rel32 to target NAME, measured from the
    end of the instruction (TAIL bytes after the operand)."""
    out=bytearray()
    for part in re.split(r'(\{[^}]*\})',value.replace(' ','')):
        if not part.startswith('{'):
            out+=bytes.fromhex(part)
            continue
        _,name,tail=part[1:-1].split(':')
        out+=struct.pack('<i',targets[name]-(at+len(out)+4+int(tail)))
    return bytes(out)


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def with_base(meta, every):
    """meta with its Base's Lines and Targets in front: a patch that is a shared base plus a few
    values (Diablo III's offline seasons: one season entry per number on top of one set of hooks).
    The base is the Metadata of that Name for the same build; its Lines come first, so a Line of the
    patch overwrites them and a Target's Line index counts from the base's first Line."""
    name=meta.get('Base')
    if not name: return meta
    parents=[m for m in every if m.get('Name')==name and m.get('AppVer')==meta.get('AppVer')]
    if not parents: raise ValueError(f"{meta.get('Name')}: its base patch {name!r} is missing")
    merged=ET.Element('Metadata',meta.attrib)
    for source in (with_base(parents[0],every),meta):
        merged.extend(list(source))
    return merged


# ---- Value parameters (see the module docstring) ----------------------------------------------------
PARAM_PLACEHOLDER=re.compile(r'\{param:([^}:]*):([^}]*)\}')
PARAM_INTS={f'{s}{bits}':(bits//8,s=='i') for s in 'ui' for bits in (8,16,32,64)}
PRESET_RESERVED={'Name','Note'}


def declared_params(meta):
    """The parameter names a Metadata declares (Params="a b c")."""
    return (meta.get('Params') or '').split()


def param_bytes(name, kind, value):
    """The bytes of one {param:NAME:TYPE} for value (a string, or a number)."""
    value=str(value)
    try:
        if kind in PARAM_INTS:
            size,signed=PARAM_INTS[kind]
            return int(value,0).to_bytes(size,'little',signed=signed)
        if kind=='f32': return struct.pack('<f',float(value))
        if kind=='f64': return struct.pack('<d',float(value))
        if kind.isdigit(): data=bytes.fromhex(value.replace(' ',''))
        elif kind[:1]=='a' and kind[1:].isdigit(): data=value.encode('ascii')
        else: raise ValueError(f'unknown parameter type {kind!r}')
    except (ValueError,OverflowError,UnicodeEncodeError) as error:
        raise ValueError(f'parameter {name}={value!r} is not a valid {kind}: {error}') from None
    if len(data)!=int(kind.lstrip('a')):
        raise ValueError(f'parameter {name}={value!r} is {len(data)} bytes, not {int(kind.lstrip("a"))} ({kind})')
    return data


def param_flag(name, value):
    """A parameter used as If= of a Line: 1/true or 0/false."""
    text=str(value).strip().lower()
    if text in ('1','true'): return True
    if text in ('0','false'): return False
    raise ValueError(f'parameter {name}={value!r} must be 1/true or 0/false (If=)')


def line_params(meta, line, values):
    """(written, Value) of a Line of a parameterised patch: If= decides whether it is written, and
    the Value has its {param:...} filled in."""
    flag=line.get('If')
    if flag:
        if flag not in declared_params(meta):
            raise ValueError(f"{meta.get('Name')}: If={flag!r} is not declared in its Params")
        if not param_flag(flag,values[flag]):
            return False,None
    text=line.get('Value','')
    if '{param:' not in text: return True,text
    def one(match):
        param,kind=match.group(1),match.group(2)
        if param not in declared_params(meta):
            raise ValueError(f"{meta.get('Name')}: {{param:{param}:{kind}}} is not declared in its Params")
        return param_bytes(param,kind,values[param]).hex()
    if line.get('Type')!='bytes':
        raise ValueError(f"{meta.get('Name')}: {{param:...}} is only for Type=\"bytes\" lines")
    return True,PARAM_PLACEHOLDER.sub(one,text)


def patch_presets(metas):
    """{preset name: (patch name, its values)} of the <Preset> entries of these Metadata."""
    found={}
    for meta in metas:
        for preset in meta.iter('Preset'):
            values={k:v for k,v in preset.attrib.items() if k not in PRESET_RESERVED}
            found.setdefault(preset.get('Name'),(meta.get('Name'),values))
    return found


def resolve_selection(names, metas, params=None):
    """[(selected name, patch name, values)] of the selected names: a preset name is its patch with the
    preset's values over params. A value no Metadata declares, or a declared one that a selected patch
    has not got, is an error."""
    params={k:str(v) for k,v in (params or {}).items()}
    declared={p for m in metas for p in declared_params(m)}
    unknown=sorted(set(params)-declared)
    if unknown:
        raise ValueError(f'unknown patch parameter(s) {unknown}: the patches declare {sorted(declared) or "none"}')
    by_name={m.get('Name'):m for m in reversed(metas)}
    presets=patch_presets(metas)
    selection=[]
    for name in names:
        patch,values=(name,{}) if name in by_name or name not in presets else presets[name]
        values={**params,**values}
        meta=by_name.get(patch)
        if meta is not None:
            missing=[p for p in declared_params(meta) if p not in values]
            if missing:
                raise ValueError(f'{name}: missing patch parameter(s) {missing} (--param NAME=VALUE)')
            bad=sorted(set(values)-set(declared_params(meta))-set(params))
            if bad:
                raise ValueError(f'{name}: its preset sets {bad}, which {patch!r} does not declare')
        selection.append((name,patch,values))
    return selection


def parameterised_lines(meta, values):
    """The Lines of a patch these values write: without the ones If= leaves out (a Target's Line=
    counts the written ones). The Lines themselves; line_value gives each one's Value."""
    return [line for line in meta.iter('Line') if line_params(meta,line,values)[0]]


def line_value(meta, line, values):
    """A Line's Value with its {param:...} filled in from values."""
    return line_params(meta,line,values)[1]


def preset_choices(xml, app_version):
    """{preset name: (patch name, values)} of the XML's Presets for this version, without the names a
    plain Metadata has (a mod's own entry of that name wins)."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    plain={m.get('Name') for m in metas}
    return {k:v for k,v in patch_presets(metas).items() if k not in plain}


def preset_patches(xml, app_version):
    """{preset name: [each Metadata of its patch as written with the preset's values]}: the plain entries
    one per value that the parameterised patch stands for (tests compare them with the former ones)."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    found={}
    for name,(patch,values) in preset_choices(xml,app_version).items():
        for meta in (m for m in metas if m.get('Name')==patch):
            plain=ET.Element('Metadata',{k:v for k,v in meta.attrib.items() if k not in ('Params','RequirementsIf')})
            plain.set('Name',name)
            note=next((p.get('Note') for m in metas if m.get('Name')==patch for p in m.iter('Preset')
                       if p.get('Name')==name and p.get('Note')),None)
            if note: plain.set('Note',note)
            ET.SubElement(plain,'PatchList').extend(ET.Element('Line',{**l.attrib,'Value':line_value(meta,l,values)})
                                                    for l in parameterised_lines(meta,values))
            plain.extend(meta.findall('Target'))
            found.setdefault(name,[]).append(plain)
    return found


def parse_params(items):
    """{name: value} of --param NAME=VALUE arguments."""
    params={}
    for item in items:
        name,sep,value=item.partition('=')
        if not sep or not name.strip():
            raise ValueError(f'--param {item!r}: expected NAME=VALUE')
        params[name.strip()]=value.strip()
    return params
# ---- end of value parameters --------------------------------------------------------------------------


def compile_patches(xml, names, app_version, segments, image=None, skipped=None, regions=None, caves=None,
                    params=None):
    """Writes of the named patches. With the game's image, each site is found by its signature
    (any build); without it, at the reference build's Address. A Line with a Target writes at that
    Target instead: data the code points to, such as a .bss variable (no bytes of its own to find),
    or a cave (see the module docstring), placed in regions (cave_regions; by default the zero tail of
    the first segment of the image, or the reference build's REFERENCE_CAVE_REGIONS without one).
    caves, when given, receives {cave name: image offset}.
    A patch whose sites are not all found (an Optional Line's may be missing: that Line is left out),
    or whose caves find no room, is left out whole, with the reason in skipped[name]. Writes that overlap (check_overlaps) raise ValueError.
    A patch may have several Metadata of the same Name, one per build of the game (Diablo III
    1.00 and 1.47): the first whose sites are all found is used. app_version None: any version.
    params: the values of parameterised patches (module docstring); a name may be a Preset's."""
    found={}
    every=[m for m in ET.parse(xml).getroot().iter('Metadata')
           if app_version in (None,m.get('AppVer')) and m.get('AppElf','eboot.bin')=='eboot.bin']
    selection=resolve_selection(names,every,params)
    wanted={patch for _,patch,_ in selection}
    for meta in every:
        if meta.get('Name') in wanted:
            found.setdefault(meta.get('Name'),[]).append((meta,with_base(meta,every)))
    missing=[n for n,patch,_ in selection if patch not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    # A selected patch that another selected one is built on (Base) is applied as part of that one.
    bases={base.get('Name') for _,patch,_ in selection for base in line_chain(found[patch][0][0],every)[1:]}
    picked=[]
    for name,patch,values in selection:
        if patch in bases: continue
        chosen=None
        for own,meta in found[patch] if image is not None else found[patch][:1]:
            lines=parameterised_lines(meta,values)
            try:
                sites=[None if line.get('Target') and not line.get('Pattern') else
                       locate_line(image,line) if image is not None else int(line.get('Address'),0)-EBOOT_BASE
                       for line in lines]
                targets=(resolve_targets(image,meta,sites) if image is not None else
                         {t.get('Name'):int(t.get('Address'),0)-EBOOT_BASE for t in meta.iter('Target')
                          if not t.get('Cave')})
                # An Optional Line not in this build is left out (after the Targets: Line= counts it).
                lines,sites=[l for l,s in zip(lines,sites) if s is not OMITTED],[s for s in sites if s is not OMITTED]
            except LookupError as error:
                last_error=error
                continue
            chosen=(name,own,meta,lines,sites,targets,values)
            break
        if chosen is None:
            if skipped is None: raise ValueError(f'{name}: {last_error}')
            skipped[name]=str(last_error)
            continue
        picked.append(chosen)
    if any(p[2].find('Cave') is not None for p in picked):
        if regions is None:
            regions=REFERENCE_CAVE_REGIONS if image is None else padding_regions(image,segments)
        layout,failed=allocate_caves([p[2] for p in sorted(picked,key=lambda p:every.index(p[1]))],regions,image)
    else:
        layout,failed={},{}
    writes=[]
    for name,own,meta,lines,sites,targets,values in picked:
        if id(meta) in failed:
            if skipped is None: raise ValueError(f'{name}: {failed[id(meta)]}')
            skipped[name]=failed[id(meta)]
            continue
        targets.update(cave_targets(meta,layout))
        layers=line_layers(own,every)
        sites=[targets[line.get('Target')] if line.get('Target') else site for line,site in zip(lines,sites)]
        for line,offset in zip(lines,sites):
            data=fill(line_value(meta,line,values),offset,targets) if '{' in line.get('Value','') else encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {offset+EBOOT_BASE:#x} is outside the eboot')
            writes.append((offset,data,name,layers[id(line)]))
    check_overlaps(writes,layout)
    if caves is not None:
        caves.update({cave:at for cave,(at,_,_) in layout.items()})
    return [(offset,data) for offset,data,_,_ in writes]


# Caves (see the module docstring). Without the image: the reference build's (1.47) code padding,
# 0x177a790-0x17847ec, and the rest of its last data page, 0x1b02e70-0x1b03000.
REFERENCE_CAVE_REGIONS={'code':(0x137a790,0x13847ec),'data':(0x1702e70,0x1703000)}
CAVE_ALIGN=16


def align(value, to=CAVE_ALIGN):
    return (value+to-1)//to*to


def padding_regions(image, segments):
    """cave_regions without the ELF headers: the zero tail of the first segment (the code segment of
    a PS4 eboot) and the rest of the page after the last one."""
    start,end=segments[0]
    end=min(end,len(image))
    used=len(image[start:end].rstrip(b'\0'))
    data=segments[-1][1]
    return {'code':(min(start+align(used),end),end),'data':(align(data),align(data,0x1000))}


def allocate_caves(metas, regions, image=None):
    """Places the caves of these patches (Metadata with their Base's, in a fixed order: the file's):
    each cave Name once, its Size rounded to CAVE_ALIGN, one after the other from the start of its
    Region ('code' by default) of regions, {region: (start, end)} image offsets. With the image, a
    block must be zero there. Returns ({name: (offset, size, region)}, {id(meta): reason} for a
    patch whose caves did not fit, which gets none). Two Sizes for one Name are an error."""
    layout,failed={},{}
    free={region:start for region,(start,_) in regions.items()}
    for meta in metas:
        mine,cursor={},dict(free)
        for cave in meta.iter('Cave'):
            name,size,region=cave.get('Name'),int(cave.get('Size'),0),cave.get('Region','code')
            known=layout.get(name) or mine.get(name)
            if known:
                if known[1:]!=(size,region):
                    raise ValueError(f"{meta.get('Name')}: cave {name} is {size} bytes ({region}), "
                                     f"another patch's is {known[1]} ({known[2]})")
                continue
            if region not in regions:
                failed[id(meta)]=f'no {region} region for its cave {name}'
                break
            at=align(cursor[region])
            if at+size>regions[region][1]:
                failed[id(meta)]=f'no room for its cave {name} ({size} bytes)'
                break
            if image is not None and any(image[at:at+size]):
                failed[id(meta)]=f'the room for its cave {name} at {at+EBOOT_BASE:#x} is not zero'
                break
            mine[name]=(at,size,region)
            cursor[region]=at+size
        else:
            layout.update(mine)
            free=cursor
    return layout,failed


def cave_targets(meta, layout):
    """The patch's caves and the places in them (Target Cave= At=) as targets: {name: image offset}."""
    targets={cave.get('Name'):layout[cave.get('Name')][0] for cave in meta.iter('Cave')}
    for target in meta.iter('Target'):
        if target.get('Cave'):
            targets[target.get('Name')]=layout[target.get('Cave')][0]+int(target.get('At','0'),0)
    return targets


def line_layers(meta, every):
    """{id(Line): layer} of a patch's Lines: 0 for its farthest Base, one more for each patch built on
    it (with_base's chain), so a Line can be told from the base Lines it may overwrite."""
    return {id(line):layer for layer,m in enumerate(reversed(line_chain(meta,every))) for line in m.iter('Line')}


def line_chain(meta, every):
    """[meta, its Base, the Base's Base...] (with_base's chain)."""
    chain=[meta]
    while chain[-1].get('Base'):
        chain.append(next(m for m in every if m.get('Name')==chain[-1].get('Base')
                          and m.get('AppVer')==chain[-1].get('AppVer')))
    return chain


def check_overlaps(writes, layout):
    """ValueError unless the writes [(offset, data, patch, layer)] keep apart: two writes may share
    bytes only within one patch, a Line overwriting its Base's (another layer; Diablo III's season
    entries write their number into the shared offline seasons cave), and a write that touches a cave
    must lie inside it."""
    spans=sorted((offset,offset+len(data),patch,layer,data) for offset,data,patch,layer in writes if data)
    for i,(start,end,patch,layer,data) in enumerate(spans):
        for other_start,other_end,other,other_layer,other_data in spans[i+1:]:
            if other_start>=end: break
            if patch!=other or layer==other_layer:
                raise ValueError(f'{other} writes {other_start+EBOOT_BASE:#x}, which {patch} also writes '
                                 f'({start+EBOOT_BASE:#x}-{end+EBOOT_BASE:#x})')
        for name,(at,size,_) in layout.items():
            if start<at+size and at<end and not (at<=start and end<=at+size):
                raise ValueError(f'{patch} writes {start+EBOOT_BASE:#x}-{end+EBOOT_BASE:#x}, '
                                 f'across the edge of the cave {name}')


PATCHES_DIR=Path(__file__).resolve().parent.parent/'patches'
# Built-in patch file per game: its title IDs, the AppVer its metadata is tagged with and the
# title that also recognizes a release under an ID not listed. The signatures find the sites in
# any region or version whose code matches, so the eboot.bin build is not checked.
GAMES=[({'CUSA07671','CUSA05670'},PATCHES_DIR/'WipEout_Omega_Collection.xml','01.00',
        'WipEout Omega Collection'),
       ({'CUSA00068','CUSA00006','CUSA00004'},PATCHES_DIR/'Knack.xml','01.10','Knack'),
       ({'CUSA00243','CUSA00242','CUSA00434','CUSA00433'},PATCHES_DIR/'Diablo_III.xml','01.47',
        'Diablo III')]
# The builds the built-in Address values and third-party patch files are written for (Knack 1.00,
# Niko's shadPS4 resolution patches, #12, and 1.10; the first is the base port's).
REFERENCE_SHA256='d85c8135d330c3b601bf5dc3f1dd86bd6119fb8a9fce372bed2c9785f9a79299'
REFERENCE_SHA256S={REFERENCE_SHA256,
                   '7c9574f72fbefa2012e13817afab94e4a785a709c2c1344195428276c398c157',
                   'b4b05b4b5cbdc2d603621e8d3f075e3fce19f9b2ff907c9496d4887195cc8f14'}
DMEM_RETAIL_MB=5056  # runtime_memory.c POOL_SIZE default


def game_sfo(game):
    """The values of the game folder's param.sfo, or {} when it cannot be read (prepare.read_sfo)."""
    if not game:
        return {}
    try:
        from prepare import read_sfo
    except ImportError:
        return {}
    return read_sfo(Path(game) / 'sce_sys/param.sfo') or {}


def game_title_id(game):
    return game_sfo(game).get('TITLE_ID')


def game_profile(title_id, title=None):
    """(title IDs, built-in XML, metadata AppVer, title) for a known game, else None."""
    title=(title or '').replace('®','').replace('™','')
    return next((g for g in GAMES if title_id in g[0] or (title and g[3] in title)),None)


def game_profile_for(game):
    """The profile of a game folder, by title ID or by the title in its param.sfo."""
    values=game_sfo(game) if game else {}
    found=game_profile(values.get('TITLE_ID'),values.get('TITLE'))
    if found is None and values:
        # Diablo III under a title ID not listed and a title in another script: its content ID.
        from game_profiles import is_diablo3_sfo
        if is_diablo3_sfo(values):
            found=next(g for g in GAMES if g[3]=='Diablo III')
    return found
_SHA_CACHE={}


def eboot_sha256(game):
    try:
        path=Path(game)/'eboot.bin'
        stat=path.stat()
        key=(str(path),stat.st_mtime_ns,stat.st_size)
        if key not in _SHA_CACHE:
            _SHA_CACHE[key]=hashlib.sha256(path.read_bytes()).hexdigest()
        return _SHA_CACHE[key]
    except (OSError, TypeError):
        return None


def game_patchable(game):
    """Whether the built-in patches are tried on this game folder (any region and version)."""
    return bool(game) and bool(game_profile_for(game))


def patch_status(out):
    """The last start's patch report (patches.py writes it): {'applied': [...], 'skipped': {...}}."""
    try:
        return json.loads((Path(out)/'patch_status.json').read_text())
    except (OSError, ValueError):
        return None


def available_patches(xml, app_version):
    """Names of the patches the XML has for this app version that can be chosen by name: the plain ones
    and the Presets of the parameterised ones (those need their values: compile_patches params)."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    return {m.get('Name') for m in metas if not declared_params(m)} | set(patch_presets(metas))


# A chosen resolution patch replaces a game's texture fix when it has one (both resize the video
# arena: enable one, never both).
EXCLUSIVE_PREFIX='Resolution Patch'
REPLACED_BY_EXCLUSIVE={'Bug Fix - Texture Corruption Fix'}
# Fixes with nothing to choose: not offered in the launcher and applied even with its saved list,
# which predates them (the startup black screen fix sets a value the game overwrites at once).
ALWAYS_ON={'Bug Fix - Startup Black Screen'}


def selected_patches(xml, app_version, extra, only=False):
    """The file's isEnabled patches for this version, then the names in extra (";"-separated).
    only: extra is the whole selection (the launcher's), without the file's defaults but with
    the ALWAYS_ON ones."""
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    names=[] if only else [m.get('Name') for m in metas if m.get('isEnabled','false').lower()=='true']
    names+=[n for n in (n.strip() for n in extra.split(';')) if n and n not in names]
    names+=[m.get('Name') for m in metas if m.get('Name') in ALWAYS_ON and m.get('Name') not in names]
    exclusive=[n for n in names if n.startswith(EXCLUSIVE_PREFIX)]
    if len(exclusive)>1:
        raise ValueError(f'choose one resolution patch, not {exclusive}')
    return [n for n in names if not (exclusive and n in REPLACED_BY_EXCLUSIVE)]


def resolution_names(xmls):
    """Sizes ("3440x1440") of the "Resolution Patch - <size>" entries of these patch files, smallest first."""
    found = []
    for path in xmls:
        try:
            root = ET.parse(path).getroot()
        except (OSError, ET.ParseError):
            continue
        for name in [m.get('Name') or '' for m in root.iter('Metadata')] + list(patch_presets(root.iter('Metadata'))):
            if name.startswith(EXCLUSIVE_PREFIX) and name[len(EXCLUSIVE_PREFIX):].startswith(' - '):
                size = name[len(EXCLUSIVE_PREFIX) + 3:].strip()
                if size and size not in found:
                    found.append(size)
    def pixels(size):
        width, _, height = size.partition('x')
        return (int(width) * int(height), size) if width.isdigit() and height.isdigit() else (1 << 60, size)
    return sorted(found, key=pixels)


def merge_patch_files(base, extras, out):
    """Writes `out`: the patches of `base` plus those of the mods' patch files (`extras`, in load order).
    A patch with a name that is already there is replaced; a file for other titles or one that does not
    parse is left out with a note. Returns `out`."""
    tree = ET.parse(base)
    root = tree.getroot()
    ids = {e.text.strip() for e in root.iter('ID') if e.text}
    for path in extras:
        try:
            extra = ET.parse(path).getroot()
        except (OSError, ET.ParseError) as error:
            print(f'Patches: {Path(path).name}: {error}', file=sys.stderr)
            continue
        extra_ids = {e.text.strip() for e in extra.iter('ID') if e.text}
        if extra_ids and not extra_ids & ids:
            continue
        for meta in extra.iter('Metadata'):
            name = meta.get('Name')
            for old in [m for m in root.iter('Metadata') if m.get('Name') == name]:
                root.remove(old)
            root.append(meta)
            print(f'Patches: {name} from the mod file {Path(path).parent.parent.name}/{Path(path).name}')
    ET.indent(tree)
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    tree.write(out, encoding='utf-8', xml_declaration=True)
    return Path(out)


def default_enabled(xmls, app_version):
    """Names of the patches of these files that are on by default (isEnabled), for this app version."""
    names = []
    for path in xmls:
        try:
            root = ET.parse(path).getroot()
        except (OSError, ET.ParseError):
            continue
        names += [m.get('Name') for m in root.iter('Metadata')
                  if m.get('AppVer') == app_version and m.get('isEnabled', 'false').lower() == 'true']
    return names


def patch_requirements(xml, names, app_version, params=None):
    """(direct memory MiB, VBlank Hz) the selected patches' notes ask for, None when not needed.
    shadPS4 notes say "Requires setting DMEM to N MB": N is on top of the retail pool. A
    parameterised patch's Metadata with RequirementsIf="NAME" asks only when that value is true."""
    dmem=vblank=None
    metas=[m for m in ET.parse(xml).getroot().iter('Metadata') if m.get('AppVer')==app_version]
    chosen=[(patch,values) for _,patch,values in resolve_selection(names,metas,params)]
    for meta in metas:
        values=next((v for patch,v in chosen if patch==meta.get('Name')),None)
        if values is None: continue
        flag=meta.get('RequirementsIf')
        if flag and not param_flag(flag,values.get(flag,'0')): continue
        note=meta.get('Note','')
        if m:=re.search(r'DMEM to (\d+) MB',note): dmem=max(dmem or 0,DMEM_RETAIL_MB+int(m.group(1)))
        if m:=re.search(r'VBlank to (\d+) FPS',note): vblank=max(vblank or 0,int(m.group(1)))
    return dmem,vblank


def external_patches(directory, app_version, exclude, ids):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml (third-party
    shadPS4/GoldHEN files in the data directory's patches/ folder).
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve() or path.resolve() in {g[1].resolve() for g in GAMES}:
            continue  # built-in files (patches/ in a checkout) are not also offered as external
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        file_ids={e.text.strip() for e in root.iter('ID') if e.text}
        if file_ids and not file_ids&ids: continue
        for meta in root.iter('Metadata'):
            # The profile's tag, or the game's own version (Knack 1.00 files on a 1.00 dump, #12).
            versions={app_version} if isinstance(app_version,str) else set(app_version)
            if meta.get('AppVer') in versions and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def write_patches(out, writes):
    # G3PATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots.
    blob=struct.pack('<8sQQ',b'G3PATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (out/'patches.bin').write_bytes(blob)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--mod-patches',type=Path,action='append',default=[],
                   help='patch file of an enabled mod (repeat; in load order)')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--param',action='append',default=[],metavar='NAME=VALUE',
                   help='a value of a parameterised patch (repeat; see the module docstring)')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('SANCTUARY_GAME_DIR','../CUSA00243')))
    a=p.parse_args()
    try:
        params=parse_params(a.param)
    except ValueError as error:
        p.error(str(error))
    title=game_title_id(a.game_dir)
    profile=game_profile_for(a.game_dir)
    if not profile:
        write_patches(a.out,[])
        print(f'Patches: no patch profile for {title}: none applied')
        return
    ids,xml,tag,_name=profile
    if a.mod_patches:
        xml=merge_patch_files(xml,a.mod_patches,a.out/'patches-merged.xml')
    sha=eboot_sha256(a.game_dir)
    print(f'Patches: {title} {game_app_version(a.game_dir)}, eboot.bin sha256 {sha}')
    names=selected_patches(xml,tag,a.extra,os.environ.get('SANCTUARY_PATCHES_ONLY')=='1')
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    skipped={}
    try:
        writes=compile_patches(xml,names,None,segments,eboot_image(elf),skipped,cave_regions(elf),params=params)
    except ValueError as error:
        sys.exit(f'Patches: {error}')
    for name,reason in skipped.items():
        print(f'Patches: {name} skipped ({reason} in this eboot.bin)')
    if a.patches_dir and sha in REFERENCE_SHA256S:
        # Third-party files use the absolute addresses of the reference build, so they are only
        # applied there. After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,{tag,game_app_version(a.game_dir)},xml,ids),
                                                    a.patches_config),segments)
    write_patches(a.out,writes)
    applied=[n for n in names if n not in skipped]
    (a.out/'patch_status.json').write_text(json.dumps({'sha256':sha,'applied':applied,'skipped':skipped}))
    if params:
        print(f'Patches: values {" ".join(f"{k}={v}" for k,v in sorted(params.items()))}')
    print(f'Patches: {title}; {len(writes)} writes from {applied or "none"}')

if __name__=='__main__':
    main()
