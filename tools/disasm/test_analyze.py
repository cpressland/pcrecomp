#!/usr/bin/env python3
"""Input validation tests for the 16-bit DOS MZ analyzer.

Run: python3 tools/disasm/test_analyze.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import Analyzer  # noqa: E402


def mz_image(*, signature=b'MZ', pages=1, last_page=64, header_paras=2,
             size=64):
    data = bytearray(size)
    if size >= 2:
        data[:2] = signature
    if size >= 10:
        struct.pack_into('<HHH', data, 2, last_page, pages, 0)
        struct.pack_into('<H', data, 8, header_paras)
    return bytes(data)


def rejects(data, message):
    try:
        Analyzer(data)
    except ValueError as exc:
        assert message in str(exc), (message, str(exc))
    else:
        raise AssertionError(f'expected ValueError containing {message!r}')


def main():
    # A 32-byte DOS header and 32-byte load module, matching the MZ size rules.
    analyzer = Analyzer(mz_image())
    assert analyzer.hdr_size == 32
    assert analyzer.img_size == 64
    full_page = Analyzer(mz_image(last_page=0, size=512))
    assert full_page.img_size == 512

    rejects(b'MZ', 'truncated MZ header')
    rejects(mz_image(signature=b'NE'), 'not an MZ executable')
    rejects(mz_image(pages=0), 'page count is zero')
    rejects(mz_image(last_page=512), 'exceeds 511')
    rejects(mz_image(header_paras=1), 'fewer than 28 bytes')
    rejects(mz_image(pages=1, last_page=16, header_paras=2),
            'header extends past image')
    rejects(mz_image(size=48), 'declared image exceeds file size')

    print('analyze: ok (valid MZ and 7 malformed-header cases)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
