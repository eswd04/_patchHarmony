#!/usr/bin/env python3
"""Recover a 6.12 ARM64 Image's symbol map and export CRCs, checking live names.

Input: uncompressed kernel payload from boot.img and /proc/kallsyms from the
same boot. Addresses in /proc/kallsyms may be hidden. Outputs are for matching
module builds and analysis, not a reconstruction of kernel source or headers.
"""
import argparse
from pathlib import Path
import struct


def align8(value):
    return (value + 7) & ~7


def cstring(data, start):
    if not 0 <= start < len(data):
        raise ValueError("string outside Image")
    end = data.index(0, start, min(start + 1024, len(data)))
    return data[start:end].decode("ascii")


def decode(data, start, names):
    count = len(names)
    pos = names_start = align8(start + 4)
    encoded, markers = [], []
    for index in range(count):
        if index % 256 == 0:
            markers.append(pos - names_start)
        length = data[pos]
        pos += 1
        if length & 128:
            length = (length & 127) | (data[pos] << 7)
            pos += 1
        if not length or pos + length > len(data):
            raise ValueError("invalid compressed name")
        encoded.append(data[pos:pos + length])
        pos += length
    marker_start = align8(pos)
    if tuple(markers) != struct.unpack_from(f"<{len(markers)}I", data, marker_start):
        raise ValueError("markers do not match compressed names")
    pos = token_start = align8(marker_start + len(markers) * 4)
    tokens, indexes = [], []
    for _ in range(256):
        indexes.append(pos - token_start)
        end = data.index(0, pos, min(pos + 1024, len(data)))
        tokens.append(data[pos:end])
        pos = end + 1
    index_start = align8(pos)
    if tuple(indexes) != struct.unpack_from("<256H", data, index_start):
        raise ValueError("token indexes do not match strings")
    decoded = [b"".join(tokens[k] for k in item).decode("ascii") for item in encoded]
    if any(item[1:] != name for item, name in zip(decoded, names)):
        raise ValueError("Image symbols differ from the running kernel")
    offset_start = align8(index_start + 512)
    offsets = struct.unpack_from(f"<{count}I", data, offset_start)
    base = struct.unpack_from("<Q", data, align8(offset_start + count * 4))[0]
    rows = [(base + offset, name[0], name[1:]) for offset, name in zip(offsets, decoded)]
    if rows[0][2] != "_text" or rows[0][0] != base:
        raise ValueError("unsupported relative-address layout")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("kallsyms", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = args.image.read_bytes()
    if data[56:60] != b"ARM\x64":
        parser.error("expected an uncompressed ARM64 Linux Image")
    names = []
    for line in args.kallsyms.read_text().splitlines():
        columns = line.split()
        if len(columns) > 3:
            break  # module symbols follow all built-in kernel symbols
        if len(columns) != 3:
            parser.error("invalid /proc/kallsyms line")
        names.append(columns[2])
    if len(names) < 10000:
        parser.error("incomplete kernel symbol list")
    needle = struct.pack("<I", len(names))
    start = 0
    candidates = []
    while True:
        start = data.find(needle, start)
        if start < 0:
            break
        if start % 8 == 0:
            try:
                candidates.append(decode(data, start, names))
            except (ValueError, IndexError, struct.error):
                pass
        start += 1
    if len(candidates) != 1:
        parser.error(f"expected one matching kallsyms table; found {len(candidates)}")
    rows = candidates[0]
    symbols = {name: addr for addr, _, name in rows}
    base = symbols["_text"]
    exports = []
    for suffix, kind in [("", "EXPORT_SYMBOL"), ("_gpl", "EXPORT_SYMBOL_GPL")]:
        start = symbols["__start___ksymtab" + suffix] - base
        stop = symbols["__stop___ksymtab" + suffix] - base
        crc_start = symbols["__start___kcrctab" + suffix] - base
        crc_stop = symbols["__stop___kcrctab" + suffix] - base
        if (stop - start) % 12 or (stop - start) // 12 * 4 != crc_stop - crc_start:
            parser.error("export and CRC table lengths differ")
        for index, pos in enumerate(range(start, stop, 12)):
            value, name, namespace = struct.unpack_from("<iii", data, pos)
            name = cstring(data, pos + 4 + name)
            namespace = cstring(data, pos + 8 + namespace)
            if symbols.get("__ksymtab_" + name) != base + pos:
                parser.error(f"export entry does not match symbol map: {name}")
            # Exported function values may point to a CFI alias, so only the
            # name and table address are required to identify its CRC entry.
            # Exported data can live in BSS, which has no bytes in Image.
            if not 0 <= pos + value < symbols["_end"] - base:
                parser.error(f"export value outside kernel address range: {name}")
            crc = struct.unpack_from("<I", data, crc_start + index * 4)[0]
            exports.append(f"0x{crc:08x}\t{name}\tvmlinux\t{kind}\t{namespace}\n")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "System.map").write_text("".join(
        f"{addr:016x} {kind} {name}\n" for addr, kind, name in rows))
    (args.output / "Module.symvers").write_text("".join(exports))
    print(f"Verified {len(rows)} live symbol names; recovered {len(exports)} export CRCs")


if __name__ == "__main__":
    main()
