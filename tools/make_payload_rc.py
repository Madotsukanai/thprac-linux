import argparse
import hashlib
import os
from pathlib import Path

FILES = [
    'thprac_bridge64.exe', 'thprac_th06nc.dll',
    'LICENCE',
    'licenses/FreeType-LICENSE.txt', 'licenses/FreeType-FTL.txt',
    'licenses/MinHook.txt', 'licenses/ImGui.txt',
    'THIRD_PARTY_NOTICES.txt',
    'thprac-injector', 'thprac_wineloader.dll',
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('payload_dir', type=Path)
    ap.add_argument('rc_out', type=Path)
    ap.add_argument('bundle_dir', type=Path)
    args = ap.parse_args()
    payload_dir = args.payload_dir
    rc_out = args.rc_out
    bundle_dir = args.bundle_dir
    bundle_dir.mkdir(parents=True, exist_ok=True)

    files = [(name, (payload_dir / name).read_bytes()) for name in FILES]
    digest = hashlib.sha256()
    for name, data in files:
        digest.update(name.encode('utf-8') + b'\0' + hashlib.sha256(data).digest())
    bundle_id = digest.hexdigest().encode('ascii')
    if len(bundle_id) != 64:
        raise SystemExit('unexpected bundle id length')

    id_path = bundle_dir / 'bundle_id.bin'
    id_path.write_bytes(bundle_id)

    def rc_path(p: Path) -> str:
        # windres accepts forward slashes
        return p.resolve().as_posix().replace('"', '\\"')

    lines = []
    lines.append(f'100 RCDATA "{rc_path(id_path)}"')
    for i, (name, _) in enumerate(files, start=101):
        fpath = (payload_dir / name).resolve()
        lines.append(f'{i} RCDATA "{rc_path(fpath)}"')
    rc_out.parent.mkdir(parents=True, exist_ok=True)
    rc_out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(bundle_id.decode())

if __name__ == '__main__':
    main()
