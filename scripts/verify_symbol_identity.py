"""Verify the PE CodeView GUID/age against the supplied MSF 7 PDB info stream."""
import argparse
from pathlib import Path
import struct


def pe_identity(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    if magic not in (0x10b, 0x20b):
        raise ValueError('Unsupported PE optional header')
    directory = optional + (112 if magic == 0x20b else 96)
    debug_rva, debug_size = struct.unpack_from('<II', data, directory + 6 * 8)
    for i in range(sections):
        section = optional + optional_size + i * 40
        size, rva, raw_size, raw = struct.unpack_from('<IIII', data, section + 8)
        if rva <= debug_rva < rva + max(size, raw_size):
            offset = raw + debug_rva - rva
            for entry in range(offset, offset + debug_size, 28):
                kind, size, _, pointer = struct.unpack_from('<IIII', data, entry + 12)
                if kind == 2 and size >= 24 and data[pointer:pointer + 4] == b'RSDS':
                    return data[pointer + 4:pointer + 20], struct.unpack_from('<I', data, pointer + 20)[0]
    raise ValueError('PE has no matching PDB CodeView identity')


def pdb_identity(data):
    if not data.startswith(b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0'):
        raise ValueError('Unsupported PDB format')
    block, _, count, directory_size, _, block_map = struct.unpack_from('<IIIIII', data, 32)
    if block not in (512, 1024, 2048, 4096, 8192) or count * block != len(data):
        raise ValueError('Invalid PDB block geometry')
    n = (directory_size + block - 1) // block
    directory_blocks = struct.unpack_from(f'<{n}I', data, block_map * block)
    directory = b''.join(data[b * block:(b + 1) * block] for b in directory_blocks)[:directory_size]
    streams = struct.unpack_from('<I', directory)[0]
    sizes = struct.unpack_from(f'<{streams}I', directory, 4)
    offset = 4 + streams * 4
    for index, size in enumerate(sizes):
        n = 0 if size == 0xffffffff else (size + block - 1) // block
        blocks = struct.unpack_from(f'<{n}I', directory, offset)
        offset += n * 4
        if index == 1:
            info = b''.join(data[b * block:(b + 1) * block] for b in blocks)[:size]
            if len(info) < 28:
                raise ValueError('Missing PDB info stream')
            return info[12:28], struct.unpack_from('<I', info, 8)[0]
    raise ValueError('No PDB info stream')


def verify(exe, pdb):
    if pe_identity(Path(exe).read_bytes()) != pdb_identity(Path(pdb).read_bytes()):
        raise ValueError(f'Stale or unrelated PDB: {pdb}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('pdb')
    args = parser.parse_args()
    verify(args.executable, args.pdb)
    print('Matching PE/PDB GUID and age')
