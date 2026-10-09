#!/usr/bin/env python3
"""Install the ZZ9000 RTG blitter into a Main_MiSTer source tree.

    zz9000/mister/install.py /path/to/Main_MiSTer

Copies the sources into support/minimig/ and applies three small, idempotent
edits:
  Makefile     C_SRC also globs support/minimig/zz9000/*.c (the official
               ZZ9000OS gfx.c is C and does not build as C++)
  user_io.cpp  zz9000_stop() next to a2065_stop() when a core is (re)loaded,
               zz9000_start() next to a2065_start() when Minimig is identified
Re-running it refreshes the copied files and leaves already-applied edits
alone.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.join(HERE, '..', 'common')


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    print('  copy', os.path.relpath(dst, TREE))


def edit(path, anchor, insert, marker, after=True):
    with open(path) as f:
        s = f.read()
    if marker in s:
        print('  ok  ', os.path.relpath(path, TREE), '(%s)' % marker.strip())
        return
    if s.count(anchor) != 1:
        sys.exit('error: %s: anchor %r found %d times' % (path, anchor, s.count(anchor)))
    s = s.replace(anchor, anchor + insert if after else insert + anchor)
    with open(path, 'w') as f:
        f.write(s)
    print('  edit', os.path.relpath(path, TREE), '(%s)' % marker.strip())


if len(sys.argv) != 2:
    sys.exit(__doc__)
TREE = os.path.abspath(sys.argv[1])
MM = os.path.join(TREE, 'support', 'minimig')
if not os.path.isfile(os.path.join(TREE, 'user_io.cpp')) or not os.path.isdir(MM):
    sys.exit('error: %s does not look like a Main_MiSTer tree' % TREE)

print('Installing ZZ9000 RTG blitter into', TREE)
copy(os.path.join(HERE, 'minimig_zz9000.cpp'), os.path.join(MM, 'minimig_zz9000.cpp'))
copy(os.path.join(HERE, 'minimig_zz9000.h'), os.path.join(MM, 'minimig_zz9000.h'))
ZZ = os.path.join(MM, 'zz9000')
for name in ('zz9000_blit.c', 'zz9000_fbmem.c', 'zz9000_fbmem.h',
             'zz9000_service.c', 'zz9000_service.h'):
    copy(os.path.join(HERE, name), os.path.join(ZZ, name))
copy(os.path.join(COMMON, 'mister_zz9000.h'), os.path.join(ZZ, 'mister_zz9000.h'))
for name in sorted(os.listdir(os.path.join(HERE, 'zz9000os'))):
    copy(os.path.join(HERE, 'zz9000os', name), os.path.join(ZZ, 'zz9000os', name))

edit(os.path.join(TREE, 'Makefile'),
     'C_SRC =   $(wildcard *.c) \\\n',
     '          $(wildcard ./support/minimig/zz9000/*.c) \\\n',
     'support/minimig/zz9000/*.c')

UIO = os.path.join(TREE, 'user_io.cpp')
edit(UIO, '#include "support/minimig/minimig_a2065.h"\n' if
     '#include "support/minimig/minimig_a2065.h"\n' in open(UIO).read()
     else '#include "support.h"\n',
     '#include "support/minimig/minimig_zz9000.h"\n',
     'minimig_zz9000.h')
edit(UIO, '\ta2065_stop();\n',
     '\n\t// ...and the ZZ9000 RTG blitter thread.\n\tzz9000_stop();\n',
     'zz9000_stop();')
edit(UIO, '\t\t\t\t\ta2065_start();\n',
     '\t\t\t\t\tzz9000_start();\n',
     'zz9000_start();')
print('done')
