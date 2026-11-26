'use strict';
const assert=require('assert/strict'),{audit}=require('../p1275-macho-audit.js');
// Thin ARM64 dylib with an actual nlist weak dynamic lookup, initializer
// and one-terminal exports trie. Synthetic fixture; no binary claimed.
const b=Buffer.alloc(512),w=(o,n)=>b.writeUInt32LE(n>>>0,o),q=(o,n)=>b.writeBigUInt64LE(BigInt(n),o);
w(0,0xfeedfacf);w(4,0x100000c);w(12,6);w(16,4);w(20,152+24+16+24);
let o=32;w(o,0x19);w(o+4,152);b.write('__DATA',o+8);q(o+32,512);q(o+40,0);q(o+48,512);w(o+64,1);
b.write('__mod_init_func',o+72);b.write('__DATA',o+88);q(o+112,8);w(o+120,400);w(o+136,9);q(400,1234);
o+=152;w(o,2);w(o+4,24);w(o+8,320);w(o+12,2);w(o+16,352);w(o+20,32);w(320,1);b[324]=0x0f;w(336,6);b[340]=1;b.writeUInt16LE(0xfe40,342);b.write('\0_vkx\0_missing\0',352);
o+=24;w(o,0x80000033);w(o+4,16);w(o+8,420);w(o+12,10);Buffer.from([0,1,95,118,0,6,2,0,0,0]).copy(b,420);
o+=16;w(o,0x1b);w(o+4,24);b.fill(1,o+8,o+24);
const r=audit(b);assert.equal(r.initializer_entries,1);assert.equal(r.symbol_table.weak_dynamic_lookup_count,1);assert.deepEqual(r.symbol_table.weak_dynamic_lookup_names,['_missing']);assert.equal(r.export_trie.count,1);assert.equal(r.export_trie.names[0],'_v');assert.equal(r.uuid,'01'.repeat(16));
assert.throws(()=>audit(Buffer.alloc(32)),/arm64/);
const bad=Buffer.from(b);bad.writeUInt32LE(9999,36);assert.throws(()=>audit(bad),/load command/);
// Independent chained fixup import and starts-in-segment fixture.
const c=Buffer.alloc(256);c.writeUInt32LE(0xfeedfacf,0);c.writeUInt32LE(0x100000c,4);c.writeUInt32LE(6,12);c.writeUInt32LE(1,16);c.writeUInt32LE(16,20);
c.writeUInt32LE(0x80000034,32);c.writeUInt32LE(16,36);c.writeUInt32LE(64,40);c.writeUInt32LE(96,44);
const u=(o,v)=>c.writeUInt32LE(v>>>0,64+o);
u(4,28);u(8,64);u(12,68);u(16,1);u(20,1);u(24,0);
u(28,1);u(32,8);u(36,24);c.writeUInt16LE(4096,104);c.writeUInt16LE(2,106);c.writeUInt16LE(1,120);c.writeUInt16LE(0,122);
u(64,0x1fe);c.write('_weak\0',132);
const cr=audit(c);assert.equal(cr.chained_fixups.imports_count,1);assert.equal(cr.chained_fixups.weak_flat_lookup_imports,1);assert.equal(cr.chained_fixups.pages[0].populated_pages,1);
console.log('P1275 Mach-O audit: synthetic ARM64 segments/initializers/weak imports/export trie PASS');
