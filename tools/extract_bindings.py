"""Extract the Lua binding table (name, state, signature, help, class) from the original
ForgedAlliance.exe. Each binding is a static object filled in by a small initializer:
  mov eax,[HEAD]; mov [B+0x10],eax; mov eax,B; mov [B+4],name; mov [B+8],state; mov [B+0xC],doc;
  mov [HEAD],eax; mov [B+0x14],func; mov [B+0x18],class; mov [B],vtable
Reference reading only; output is a TSV of names/docs used to plan our own implementation."""
import pefile, capstone, struct, re, sys, json, collections
pe = pefile.PE(sys.argv[1], fast_load=True)
d = pe.__data__
IB = pe.OPTIONAL_HEADER.ImageBase
def va2off(v):
    try: return pe.get_offset_from_rva(v - IB)
    except Exception: return None
def cstr(v):
    o = va2off(v)
    if o is None or v < IB: return None
    e = d.find(b'\0', o)
    if e < 0 or e - o > 4000: return None
    try: return d[o:e].decode('latin1')
    except Exception: return None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False
out = []
for m in re.finditer(rb'\xA1(....)\xA3(....)\xB8(....)', d, re.S):
    head, nxt, B = struct.unpack('<III', m.group(1) + m.group(2) + m.group(3))
    if nxt != B + 0x10: continue
    fields = {}
    off = m.end()
    va = pe.get_rva_from_offset(off) + IB
    for ins in md.disasm(d[off:off + 120], va):
        if ins.mnemonic == 'ret': break
        mm = re.match(r'dword ptr \[(0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+|eax)$', ins.op_str)
        if ins.mnemonic == 'mov' and mm:
            addr = int(mm.group(1), 16)
            if mm.group(2) == 'eax': continue
            fields[addr - B] = int(mm.group(2), 0)
    name = cstr(fields.get(4, 0)); state = cstr(fields.get(8, 0)); doc = cstr(fields.get(0xC, 0))
    cls = fields.get(0x18, 0)
    if not name: continue
    out.append(dict(head=hex(head), obj=hex(B), name=name, state=state, doc=doc, func=hex(fields.get(0x14, 0)),
                    cls_ptr=hex(cls), vt=hex(fields.get(0, 0))))
# class names: the class field points at a class-binder object; resolve its name via the same table
by_obj = {o['obj']: o for o in out}
heads = collections.Counter(o['head'] for o in out)
json.dump(out, open(sys.argv[2], 'w'), indent=0)
print(len(out), 'bindings'); print(heads.most_common(10))
print(collections.Counter(o['state'] for o in out).most_common(10))
