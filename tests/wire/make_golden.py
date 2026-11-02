#!/usr/bin/env python3
"""Writes the golden frames from docs/wire-protocol.md alone, with no line of the library's code,
so expected bytes never come from the encoder they test. Run it from anywhere, and commit its
output, which .gitattributes keeps byte-exact."""
import pathlib
import struct

TYPE = {
    'Hello': 1, 'Welcome': 2, 'Command': 3, 'Refused': 4, 'ProtocolError': 5,
    'OrderAccepted': 16, 'OrderRejected': 17, 'TradeExecuted': 18, 'OrderCancelled': 19,
    'CancelRejected': 20, 'OrderExpired': 21, 'SelfTradePrevented': 22, 'StopTriggered': 23,
    'OrderReplaced': 24, 'ReplaceRejected': 25,
}
BUY, SELL = 1, 2
CORRELATION = 0x1122334455667788
LAST = 1


def frame(name, body, flags=0, correlation=CORRELATION):
    return struct.pack('<HBBQ', 12 + len(body), TYPE[name], flags, correlation) + body


def symbol(text):
    return text.ljust(8).encode('ascii')


def command(kind, side, order_type, tif, name, order_id, price, quantity, stop, owner, flags, mode):
    return (struct.pack('<BBBB', kind, side, order_type, tif) + symbol(name) +
            struct.pack('<IIIII', order_id, price, quantity, stop, owner) +
            struct.pack('<BBH', flags, mode, 0))


FRAMES = {
    'hello': frame('Hello', b'OBWIRE\r\n' + struct.pack('<HH', 1, 0)),
    'welcome': frame('Welcome', b'OBWIRE\r\n' + struct.pack('<HH', 1, 0), LAST),
    'command_add': frame('Command', command(1, BUY, 1, 1, 'AAPL', 7, 100, 5, 0, 9, 1, 2)),
    'command_stop': frame('Command', command(1, SELL, 4, 2, 'BRK.B', 8, 95, 3, 96, 0, 0, 0)),
    'command_cancel': frame('Command', command(2, 0, 0, 0, 'ES-Z26', 77, 0, 0, 0, 0, 0, 0)),
    'command_replace': frame('Command', command(3, 0, 0, 0, 'AAPL', 7, 101, 6, 0, 9, 0, 3)),
    'refused': frame('Refused', symbol('NOPE') + struct.pack('<IHH', 7, 1, 0), LAST),
    'protocol_error': frame('ProtocolError', struct.pack('<HH', 8, 1), LAST),
    'order_accepted': frame('OrderAccepted', symbol('AAPL') + struct.pack('<IIIBBBB', 7, 100, 5, BUY, 0, 0, 0)),
    'order_rejected': frame('OrderRejected', symbol('AAPL') + struct.pack('<IHH', 7, 1, 0), LAST),
    'trade_executed': frame('TradeExecuted', symbol('AAPL') + struct.pack('<IIII', 7, 3, 100, 2)),
    'order_cancelled': frame('OrderCancelled', symbol('AAPL') + struct.pack('<II', 7, 5), LAST),
    'cancel_rejected': frame('CancelRejected', symbol('AAPL') + struct.pack('<IHH', 7, 2, 0), LAST),
    'order_expired': frame('OrderExpired', symbol('AAPL') + struct.pack('<II', 7, 4), LAST),
    'self_trade_prevented': frame('SelfTradePrevented', symbol('AAPL') + struct.pack('<IIII', 7, 3, 100, 6)),
    'stop_triggered': frame('StopTriggered', symbol('AAPL') + struct.pack('<III', 7, 96, 97)),
    'order_replaced': frame('OrderReplaced', symbol('AAPL') + struct.pack('<IIIIBBBB', 7, 101, 6, 4, SELL, 1, 0, 0)),
    'replace_rejected': frame('ReplaceRejected', symbol('AAPL') + struct.pack('<IHH', 7, 17, 0), LAST),
}


def main():
    here = pathlib.Path(__file__).resolve().parent / 'frames'
    here.mkdir(exist_ok=True)
    for name, bytes_ in FRAMES.items():
        (here / (name + '.frame')).write_bytes(bytes_)


if __name__ == '__main__':
    main()
