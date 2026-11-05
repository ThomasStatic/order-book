#!/usr/bin/env python3
"""Writes the golden book images from docs/checkpoint-format.md alone, with no line of the
library's code, so expected bytes never come from the encoder they test. Each book's records are
listed by hand from the commands checkpoint_tests runs to build it. Run it from anywhere, and
commit its output, which .gitattributes keeps byte-exact."""
import pathlib
import struct

BUY, SELL = 1, 2
LIMIT, MARKET, STOP, STOP_LIMIT = 1, 2, 3, 4
GTC, IOC, FOK = 1, 2, 3
CANCEL_RESTING, CANCEL_INCOMING, CANCEL_BOTH = 1, 2, 3
NONE, PARTIAL = 1, 2
MAX = 0xFFFFFFFF


def order(order_id, sequence, initial, remaining, owner=0, fill=NONE):
    return struct.pack('<IIIIIB3x', order_id, sequence, initial, remaining, owner, fill)


def level(price, orders):
    aggregate = sum(struct.unpack_from('<I', record, 12)[0] for record in orders)
    return struct.pack('<IIQ', price, len(orders), aggregate) + b''.join(orders)


def stop(order_id, sequence, stop_price, limit_price, quantity, side, order_type, tif,
         owner=0, mode=CANCEL_RESTING, post_only=0):
    return (struct.pack('<IIIIII', order_id, sequence, stop_price, limit_price, quantity, owner) +
            struct.pack('<BBBBB3x', side, order_type, tif, mode, post_only))


def image(next_sequence, next_trade, last_trade, bids=(), asks=(), stops=()):
    orders = sum(struct.unpack_from('<I', lvl, 4)[0] for lvl in (*bids, *asks))
    header = struct.pack('<16I', 1, 1, 64, 16, 24, 32, next_sequence, next_trade, last_trade,
                         len(bids), len(asks), orders, len(stops), orders, len(stops), 0)
    return header + b''.join(bids) + b''.join(asks) + b''.join(stops)


IMAGES = {
    # A new book.
    'empty': image(1, 1, 0),
    # Order 1 bids 10 at 100, order 2 bids 5 at 99 for owner 7, order 3 bids 4 at 100, order 4
    # offers 8 at 103, then order 5 sells 6 at 100 IOC: trade 1 takes 6 from order 1.
    'resting': image(6, 2, 100,
                     bids=[level(100, [order(1, 1, 10, 4, fill=PARTIAL), order(3, 3, 4, 4)]),
                           level(99, [order(2, 2, 5, 5, owner=7)])],
                     asks=[level(103, [order(4, 4, 8, 8)])]),
    # Order 1 offers 5 at 100 and order 2 buys 2 of it (trade 1), then five stops: 3 a buy STOP
    # IOC at 105, 4 a buy STOP_LIMIT GTC at 102 limit 107 for owner 9 with CANCEL_BOTH, 5 a sell
    # STOP FOK at 95, 6 a sell STOP_LIMIT IOC at 97 limit 90 with CANCEL_INCOMING, and 7 a buy
    # STOP IOC at 102.
    'stops': image(8, 2, 100,
                   asks=[level(100, [order(1, 1, 5, 3, fill=PARTIAL)])],
                   stops=[stop(4, 4, 102, 107, 1, BUY, STOP_LIMIT, GTC, owner=9, mode=CANCEL_BOTH),
                          stop(7, 7, 102, 0, 1, BUY, STOP, IOC),
                          stop(3, 3, 105, 0, 4, BUY, STOP, IOC),
                          stop(6, 6, 97, 90, 3, SELL, STOP_LIMIT, IOC, mode=CANCEL_INCOMING),
                          stop(5, 5, 95, 0, 2, SELL, STOP, FOK)]),
    # Two maximal bids at the highest price, so the aggregate is wider than one quantity.
    'maximal': image(3, 1, 0,
                     bids=[level(MAX, [order(MAX, 1, MAX, MAX, owner=MAX), order(MAX - 1, 2, MAX, MAX)])]),
}


def main():
    here = pathlib.Path(__file__).resolve().parent / 'images'
    here.mkdir(exist_ok=True)
    for name, bytes_ in IMAGES.items():
        (here / (name + '.image')).write_bytes(bytes_)


if __name__ == '__main__':
    main()
