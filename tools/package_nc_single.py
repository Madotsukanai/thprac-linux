"""Embed the native NC payload into the launcher (Windows or cross-platform via pefile-less manual update)."""
import argparse
import hashlib
import shutil
import struct
import tempfile
from pathlib import Path

FILES = [
    'thprac_bridge64.exe', 'thprac_th06nc.dll', 'freetype.dll',
    'LICENCE',
    'licenses/FreeType-LICENSE.txt', 'licenses/FreeType-FTL.txt',
    'licenses/MinHook.txt', 'licenses/ImGui.txt',
    'THIRD_PARTY_NOTICES.txt',
    'thprac-injector', 'thprac_wineloader.dll',
]

def package_windows(launcher, files, bundle_id, output):
    import ctypes as c
    from ctypes import wintypes as w
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.BeginUpdateResourceW.argtypes = [w.LPCWSTR, w.BOOL]
    kernel.BeginUpdateResourceW.restype = w.HANDLE
    kernel.UpdateResourceW.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, w.WORD, c.c_void_p, w.DWORD]
    kernel.EndUpdateResourceW.argtypes = [w.HANDLE, w.BOOL]
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix='.exe', delete=False) as tmp:
        temporary = Path(tmp.name)
    update = None
    try:
        shutil.copyfile(launcher, temporary)
        update = kernel.BeginUpdateResourceW(str(temporary.resolve()), False)
        if not update:
            raise c.WinError(c.get_last_error())
        for resource_id, data in enumerate([bundle_id.encode('ascii')] + [data for _, data in files], 100):
            buffer = c.create_string_buffer(data)
            if not kernel.UpdateResourceW(update, 10, resource_id, 0, buffer, len(data)):
                raise c.WinError(c.get_last_error())
        handle, update = update, None
        if not kernel.EndUpdateResourceW(handle, False):
            raise c.WinError(c.get_last_error())
        temporary.replace(output)
    finally:
        if update:
            kernel.EndUpdateResourceW(update, True)
        temporary.unlink(missing_ok=True)

def build_rsrc(entries):
    # entries: list of (type_id, name_id, lang, data_bytes)
    def align4(buf):
        while len(buf) % 4: buf += b'\x00'
    # build tree for one type (10/RT_RCDATA)
    # Resource directory: characteristics, time, vermaj, vermin, named_count, id_count
    # entries sorted
    by_type = {}
    for t,n,l,d in entries:
        by_type.setdefault(t, {}).setdefault(n, {})[l]=d
    # only RT_RCDATA(10)
    type_id = 10
    names = sorted(by_type[type_id].keys())
    # layout: root dir + type entry -> name dir per name? simpler: root has 1 type entry (id 10), that points to dir with N name entries, each points to dir with 1 lang entry pointing to data entry
    # We'll construct sequentially.
    root_dir = struct.pack('<IIHHHH',0,0,0,0,0,1)
    # root entry: Name/ID (high bit 0 for id), OffsetToData (high bit 1 for subdir)
    # compute offsets relative to start of resource section
    # structure:
    # [root dir (16)] [root entry (8)] [name table dir (16)] [name entries (8*N)] [lang dirs (16*N)] [lang entries (8*N)] [data entries (16*N)] [data blobs aligned]
    N = len(names)
    off = 0
    root_dir_off = off; off += 16 + 8
    name_dir_off = off; off += 16 + 8*N
    lang_dirs_off = off; off += N*(16 + 8)
    data_entries_off = off; off += N*16
    strings_off = off # none
    data_off = off
    # precompute data blob offsets
    data_offsets = []
    for name in names:
        lang = next(iter(by_type[type_id][name].keys()))
        data = by_type[type_id][name][lang]
        data_offsets.append(data_off)
        data_off += len(data)
        while data_off % 4: data_off += 1
    # root dir
    out = bytearray()
    out += struct.pack('<IIHHHH',0,0,0,0,0,1)
    out += struct.pack('<II', type_id, (1<<31) | name_dir_off)
    # name dir
    out += struct.pack('<IIHHHH',0,0,0,0,0,N)
    for i,name in enumerate(names):
        out += struct.pack('<II', name, (1<<31) | (lang_dirs_off + i*(16+8)))
    # lang dirs and entries
    for i,name in enumerate(names):
        lang = next(iter(by_type[type_id][name].keys()))
        out += struct.pack('<IIHHHH',0,0,0,0,0,1)
        out += struct.pack('<II', lang, data_entries_off + i*16)
    # data entries (OffsetToData is RVA - filled by caller? we need base; easier produce .rsrc section with VA base placeholder? but writing raw is hard)
    # Actually easier: generate .rc and use windres if available? on mingw we have i686-w64-mingw32-windres.
    return None

def package(launcher, payload_dir, output):
    files = [(name, (payload_dir / name).read_bytes()) for name in FILES]
    digest = hashlib.sha256()
    for name, data in files:
        digest.update(name.encode('utf-8') + b'\0' + hashlib.sha256(data).digest())
    bundle_id = digest.hexdigest()
    try:
        package_windows(launcher, files, bundle_id, output)
    except Exception as e:
        raise RuntimeError(f'Packaging failed (Windows UpdateResource API unavailable or error): {e}')
    print(f'Single EXE: {output} ({output.stat().st_size:,} bytes), bundle {bundle_id}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--payload-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.launcher, args.payload_dir, args.output)
