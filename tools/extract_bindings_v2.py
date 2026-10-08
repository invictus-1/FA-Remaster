"""Extract the Lua binding table from the original ForgedAlliance.exe (reference reading only).
v2 also finds initializers where the name and doc share one string (v1 missed 47 bindings, e.g.
all Issue* commands). Merges into an existing bindings JSON.
usage: extract_bindings_v2.py ForgedAlliance.exe bindings.json"""
import pefile, capstone, struct, re, json, collections, sys
pe=pefile.PE(sys.argv[1],fast_load=True); IB=pe.OPTIONAL_HEADER.ImageBase; d=pe.__data__
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
HEADS={0xf5a124:'sim',0xf59690:'user',0xf59680:'core',0xf596c8:'init'}
def cstr(v):
    try: o=pe.get_offset_from_rva(v-IB)
    except Exception: return None
    e=d.find(b'\0',o)
    return d[o:e].decode('latin1') if 0<=e-o<4000 else None
out={}
for h in HEADS:
    for m in re.finditer(re.escape(b'\xa1'+struct.pack('<I',h)), d):
        off=m.start()
        # function start: scan back to int3 padding
        s=off
        while s>0 and not (d[s-1] in (0xcc,0x90) and d[s-2] in (0xcc,0x90)) and off-s<64: s-=1
        va=pe.get_rva_from_offset(s)+IB
        eax=None; stores={}; B=None
        for ins in md.disasm(d[s:s+200], va):
            if ins.mnemonic=='ret': break
            mm=re.match(r'eax, (0x[0-9a-f]+)$', ins.op_str)
            if ins.mnemonic=='mov' and mm: eax=int(mm.group(1),16); continue
            if ins.mnemonic=='mov' and ins.op_str.startswith('eax, dword ptr'): eax=None; continue
            mm=re.match(r'dword ptr \[(0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+|eax)$', ins.op_str)
            if ins.mnemonic=='mov' and mm:
                addr=int(mm.group(1),16); v=mm.group(2)
                if addr==h and v=='eax': B=eax
                else: stores[addr]= eax if v=='eax' else int(v,0)
        if B is None: continue
        f={a-B:v for a,v in stores.items() if v is not None and 0<=a-B<0x40}
        name=cstr(f.get(4,0)) if f.get(4) else None
        if not name: continue
        out[B]=dict(set=HEADS[h], obj=hex(B), name=name, state=cstr(f.get(8,0)) if f.get(8) else None, func=hex(f.get(0x14,0)), cls=hex(f.get(0x18,0)))
old=json.load(open(sys.argv[2]))
oldobjs={int(x['obj'],16) for x in old}
new=[v for k,v in out.items() if k not in oldobjs]
print(len(out),'found;',len(new),'new'); print(collections.Counter((x['set'],x['state']) for x in new))
print(sorted(x['name'] for x in new if x['set']=='sim'))
json.dump(list(out.values()), open(sys.argv[2] + '.v2.json','w'), indent=0)
