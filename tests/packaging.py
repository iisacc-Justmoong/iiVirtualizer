#!/usr/bin/env python3
import gzip
from pathlib import Path
import stat
import struct
import sys
sys.dont_write_bytecode=True

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from prepare_guest import cpio_bytes,unpack_cpio,static_aarch64_elf

module = Path(__file__).resolve().parents[1]
account_package = 'iiAccountManagerNative'
assert 'find_dependency('+account_package+' 0.2 CONFIG)' in (module/'cmake/iiVirtualizerConfig.cmake.in').read_text()
assert '#include <iiAccountManager/NativeAccount.h>' in (module/'src/iiVirtualizer/AccountSession.cpp').read_text()

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
elf=bytearray(120);elf[:6]=b'\x7fELF\x02\x01';struct.pack_into('<HH',elf,16,2,183);struct.pack_into('<Q',elf,32,64);struct.pack_into('<HH',elf,54,56,1);struct.pack_into('<I',elf,64,1)
assert static_aarch64_elf(bytes(elf))==bytes(elf)
dynamic=bytearray(elf);struct.pack_into('<I',dynamic,64,3)
foreign=bytearray(elf);struct.pack_into('<H',foreign,18,62)
for binary in (b'#!/bin/sh\n',bytes(elf[:70]),bytes(dynamic),bytes(foreign)):
    try:static_aarch64_elf(binary)
    except ValueError:pass
    else:raise AssertionError('Unconfined/dynamic/foreign guest program accepted')
print('newc modes, contents, device entries, path boundaries, static ELF and reproducibility passed')
