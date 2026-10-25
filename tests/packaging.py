#!/usr/bin/env python3
import gzip
from pathlib import Path
import stat
import sys
sys.dont_write_bytecode=True

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_guest import cpio_bytes,unpack_cpio

entries=[('etc',stat.S_IFDIR|0o755,b''),('etc/os-release',stat.S_IFREG|0o644,b'NAME=iiOS\n'),
         ('bin/sh',stat.S_IFLNK|0o777,b'busybox'),('dev/console',stat.S_IFCHR|0o600,b'')]
archive=cpio_bytes(entries)
decoded=unpack_cpio(archive)
for name,mode,body in entries:assert decoded[name]==(mode,body)
assert gzip.compress(archive,mtime=0)==gzip.compress(cpio_bytes(entries),mtime=0)
for name in ['../escape','/absolute','x/../../escape']:
    try:unpack_cpio(cpio_bytes([(name,stat.S_IFREG|0o644,b'bad')]))
    except ValueError:pass
    else:raise AssertionError('Unsafe archive path accepted')
print('newc modes, contents, device entries, path boundaries and reproducibility passed')
