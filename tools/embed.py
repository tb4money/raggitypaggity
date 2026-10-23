#!/usr/bin/env python3
"""embed.py <in> <out.h> <symbol>: turn a file into a C byte array."""
import sys
d = open(sys.argv[1], 'rb').read()
sym = sys.argv[3]
with open(sys.argv[2], 'w') as o:
    o.write('static const unsigned char %s[] = {' % sym)
    o.write(','.join(str(b) for b in d))
    o.write('};\nstatic const unsigned int %s_LEN = %d;\n' % (sym, len(d)))
