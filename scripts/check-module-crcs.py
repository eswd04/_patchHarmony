#!/usr/bin/env python3
"""Check ARM64 module imports and version CRCs against extracted kernel exports."""
import argparse
from pathlib import Path
import struct


def cstring(data, offset):
    return data[offset:data.index(0, offset)].decode("ascii")


def check(path, exports, release):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError("expected little-endian ARM64 ELF64")
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size)
               for i in range(count)]
    name_header = headers[names_index]
    names = data[name_header[4]:name_header[4] + name_header[5]]
    sections = {cstring(names, header[0]):
                data[header[4]:header[4] + header[5]]
                for header in headers if header[1] != 8}
    metadata = sections[".modinfo"].split(b"\0")
    magic = next(x.decode().split("=", 1)[1] for x in metadata if x.startswith(b"vermagic="))
    if release and magic.split()[0] != release:
        raise ValueError(f"vermagic release mismatch: {magic}")
    versions = {}
    basic = sections.get("__versions", b"")
    if len(basic) % 64:
        raise ValueError("malformed basic CRC table")
    for pos in range(0, len(basic), 64):
        crc = struct.unpack_from("<Q", basic, pos)[0]
        versions[cstring(basic, pos + 8)] = crc
    if "__version_ext_names" in sections:
        ext_names = sections["__version_ext_names"].rstrip(b"\0").split(b"\0")
        ext_crcs = sections["__version_ext_crcs"]
        if len(ext_crcs) != len(ext_names) * 4:
            raise ValueError("extended name/CRC counts differ")
        for pos, name in enumerate(ext_names):
            name = name.decode()
            crc = struct.unpack_from("<I", ext_crcs, pos * 4)[0]
            if name in versions and versions[name] != crc:
                raise ValueError(f"basic/extended CRCs differ for {name}")
            versions[name] = crc
    if "module_layout" not in versions:
        raise ValueError("module_layout CRC missing")
    for name, crc in versions.items():
        if name not in exports:
            raise ValueError(f"versioned symbol not exported by kernel: {name}")
        if crc != exports[name]:
            raise ValueError(f"CRC mismatch for {name}: module={crc:#x}, kernel={exports[name]:#x}")
    imports = set()
    for header in headers:
        if header[1] != 2:
            continue
        strings_header = headers[header[6]]
        strings = data[strings_header[4]:strings_header[4] + strings_header[5]]
        for pos in range(header[4], header[4] + header[5], header[9]):
            name, info, _, section, _, _ = struct.unpack_from("<IBBHQQ", data, pos)
            if name and not section and info >> 4 == 1:
                imports.add(cstring(strings, name))
    missing = imports - versions.keys()
    if missing:
        raise ValueError("imports without CRC: " + ", ".join(sorted(missing)))
    print(f"PASS {path.name}: {len(imports)} imports, {len(versions)} matching CRCs; {magic}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("symvers", type=Path)
    parser.add_argument("modules", type=Path, nargs="+")
    parser.add_argument("--release", help="also require this exact vermagic release")
    args = parser.parse_args()
    exports = {row.split()[1]: int(row.split()[0], 16)
               for row in args.symvers.read_text().splitlines()}
    failures = []
    for path in args.modules:
        try:
            check(path, exports, args.release)
        except (ValueError, KeyError, IndexError, struct.error, StopIteration) as error:
            failures.append(f"FAIL {path}: {error}")
    if failures:
        parser.exit(1, "\n".join(failures) + "\n")


if __name__ == "__main__":
    main()
