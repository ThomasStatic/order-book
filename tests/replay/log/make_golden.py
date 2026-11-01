#!/usr/bin/env python3
"""Writes the golden command logs in this directory.

Expected bytes must not come from the encoder under test, so this holds its own XXH64 and its
own layout, written from docs/command-log-format.md, and its records are listed by hand. It
imports nothing outside the standard library. Rerun it only when the format changes: the files
are committed, marked -text, so CI on Linux and Windows proves they read the same.

    python tests/replay/log/make_golden.py
"""
import pathlib
import struct

MASK = (1 << 64) - 1
P1 = 0x9E3779B185EBCA87
P2 = 0xC2B2AE3D27D4EB4F
P3 = 0x165667B19E3779F9
P4 = 0x85EBCA77C2B2AE63
P5 = 0x27D4EB2F165667C5


def rotl(value, bits):
    return ((value << bits) | (value >> (64 - bits))) & MASK


def round_(accumulator, word):
    return (rotl((accumulator + word * P2) & MASK, 31) * P1) & MASK


def merge(hash_, accumulator):
    return (((hash_ ^ round_(0, accumulator)) * P1) + P4) & MASK


def xxh64(data, seed=0):
    at = 0
    size = len(data)
    if size >= 32:
        v1, v2, v3, v4 = (seed + P1 + P2) & MASK, (seed + P2) & MASK, seed, (seed - P1) & MASK
        while size - at >= 32:
            a, b, c, d = struct.unpack_from('<4Q', data, at)
            v1, v2, v3, v4 = round_(v1, a), round_(v2, b), round_(v3, c), round_(v4, d)
            at += 32
        hash_ = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & MASK
        for v in (v1, v2, v3, v4):
            hash_ = merge(hash_, v)
    else:
        hash_ = (seed + P5) & MASK
    hash_ = (hash_ + size) & MASK
    while size - at >= 8:
        word = struct.unpack_from('<Q', data, at)[0]
        hash_ = ((rotl(hash_ ^ round_(0, word), 27) * P1) + P4) & MASK
        at += 8
    if size - at >= 4:
        word = struct.unpack_from('<I', data, at)[0]
        hash_ = ((rotl(hash_ ^ ((word * P1) & MASK), 23) * P2) + P3) & MASK
        at += 4
    while at < size:
        hash_ = (rotl(hash_ ^ ((data[at] * P5) & MASK), 11) * P1) & MASK
        at += 1
    hash_ ^= hash_ >> 33
    hash_ = (hash_ * P2) & MASK
    hash_ ^= hash_ >> 29
    hash_ = (hash_ * P3) & MASK
    hash_ ^= hash_ >> 32
    return hash_


COMMAND = {'ADD': 1, 'CANCEL': 2, 'REPLACE': 3}
SIDE = {'': 0, 'BUY': 1, 'SELL': 2}
TYPE = {'': 0, 'LIMIT': 1, 'MARKET': 2, 'STOP': 3, 'STOP_LIMIT': 4}
TIF = {'': 0, 'GTC': 1, 'IOC': 2, 'FOK': 3}
MODE = {'': 0, 'CANCEL_RESTING': 1, 'CANCEL_INCOMING': 2, 'CANCEL_BOTH': 3}


def header(first_position=0, shard_count=0, shard_index=0, symbols=(), engine=(2, 1, 0)):
    body = b'OBCLOG\r\n'
    body += struct.pack('<IIII', 1, 1, 64, 40)
    body += struct.pack('<HHH', *engine) + b'\0\0'
    body += struct.pack('<QIII', first_position, shard_count, shard_index, len(symbols)) + bytes(12)
    assert len(body) == 64
    for symbol in symbols:
        body += symbol.ljust(8).encode('ascii')
    return body + struct.pack('<Q', xxh64(body))


def record(command, order_id, symbol, side='', price=0, quantity=0, order_type='', tif='', stop=0,
           post_only=False, owner=0, mode=''):
    body = struct.pack('<BBBB', COMMAND[command], SIDE[side], TYPE[order_type], TIF[tif])
    body += symbol.ljust(8).encode('ascii')
    body += struct.pack('<IIIII', order_id, price, quantity, stop, owner)
    body += struct.pack('<BB', 1 if post_only else 0, MODE[mode]) + b'\0\0'
    assert len(body) == 36
    return body


def log(head, records):
    out = head
    checksum = struct.unpack_from('<I', head, len(head) - 8)[0]
    for body in records:
        checksum = xxh64(body, checksum) & 0xFFFFFFFF
        out += struct.pack('<I', checksum) + body
    return out


EVERY_COMMAND = [
    record('ADD', 1, 'AAPL', 'SELL', 101, 5, 'LIMIT', 'GTC'),
    record('ADD', 1, 'BRK.B', 'BUY', 100, 3, 'LIMIT', 'GTC', post_only=True),
    record('ADD', 2, 'AAPL', 'BUY', 101, 2, 'LIMIT', 'IOC', owner=7, mode='CANCEL_INCOMING'),
    record('ADD', 3, 'AAPL', 'BUY', 0, 1, 'MARKET', 'FOK'),
    record('ADD', 4, 'AAPL', 'SELL', 0, 2, 'STOP', 'IOC', stop=90),
    record('ADD', 5, 'AAPL', 'BUY', 120, 2, 'STOP_LIMIT', 'GTC', stop=110, owner=7, mode='CANCEL_BOTH'),
    record('REPLACE', 1, 'AAPL', price=102, quantity=6, owner=9, mode='CANCEL_RESTING'),
    record('REPLACE', 1, 'BRK.B', price=99, quantity=3, post_only=True),
    record('CANCEL', 5, 'AAPL'),
    record('CANCEL', 77, 'ES-Z26'),
]

PLACEMENT = [
    record('ADD', 1, 'AAPL', 'SELL', 101, 5, 'LIMIT', 'GTC'),
    record('CANCEL', 1, 'AAPL'),
]


def main():
    here = pathlib.Path(__file__).parent
    (here / 'every_command.oblog').write_bytes(log(header(), EVERY_COMMAND))
    (here / 'placement.oblog').write_bytes(
        log(header(first_position=1000, shard_count=4, shard_index=2, symbols=('AAPL', 'MSFT')), PLACEMENT))
    (here / 'header_only.oblog').write_bytes(log(header(), []))
    # Two good records, then a third cut short: what a crash mid-write leaves.
    (here / 'torn_tail.oblog').write_bytes(log(header(), PLACEMENT + EVERY_COMMAND[:1])[:-7])


if __name__ == '__main__':
    main()
