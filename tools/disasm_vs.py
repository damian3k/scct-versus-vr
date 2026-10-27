"""Disassembles D3D9 shader bytecode dumped by the mod (SCCT_VR_vs_*.bin) with D3DCompiler_47.

usage: python disasm_vs.py file.bin [...]   (writes file.asm next to each)
"""
import ctypes
import sys

d3dc = ctypes.WinDLL("D3DCompiler_47.dll")


class Blob(ctypes.Structure):
    pass


def disassemble(code):
    blob = ctypes.c_void_p()
    hr = d3dc.D3DDisassemble(code, len(code), 0, None, ctypes.byref(blob))
    if hr != 0:
        raise OSError(f"D3DDisassemble failed 0x{hr & 0xFFFFFFFF:08X}")
    # ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    vtbl = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    text = ctypes.string_at(get_ptr(blob), get_size(blob)).decode("ascii", "replace")
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtbl[2])(blob)
    return text.rstrip("\0")


for path in sys.argv[1:]:
    with open(path, "rb") as f:
        text = disassemble(f.read())
    out = path.rsplit(".", 1)[0] + ".asm"
    with open(out, "w") as f:
        f.write(text)
    print(f"--- {path}\n{text}")
