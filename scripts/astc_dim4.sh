#!/bin/bash
# 装机前必查：ASTC 4x4 建图要求基级宽高是 4 的倍数（VUID-VkImageCreateInfo-extent-02252）。
# 逐张读转换清单里的 .tex 头（宽 u16LE@8、高@10），统计有多少张基级宽或高不是 4 的倍数。
set -e
E=/mnt/g/ds-hap/entry/src/main/resources/resfile/romfs
D=/mnt/g/ds-hap/ds2/src/main/resources/resfile/romfs
node - "$E" "$D" <<'JS'
const fs=require('fs'),path=require('path');
const roots=[process.argv[2],process.argv[3]];
function walk(d,out){for(const e of fs.readdirSync(d,{withFileTypes:true})){const p=path.join(d,e.name);
  if(e.isDirectory())walk(p,out);else if(e.name.endsWith('.tex'))out.push(p);}return out;}
let tot=0,bad=[],cls2=0;
for(const r of roots){let files=[];try{walk(r,files);}catch(e){continue;}
  for(const f of files){const b=fs.readFileSync(f);if(b.length<12||b.toString('ascii',0,4)!=='KTEX')continue;
    const cls=(b[4]>>4)&7;if(cls!==2)continue;cls2++;
    const w=b.readUInt16LE(8),h=b.readUInt16LE(10);tot++;
    if((w%4)||(h%4))bad.push([path.relative(r,f),w,h]);}}
console.log('类2(BC3) 张数='+cls2+'  基级非4倍数的张数='+bad.length);
for(const [f,w,h] of bad.slice(0,40))console.log('  非4倍数 '+f+'  '+w+'x'+h);
if(bad.length>40)console.log('  …还有 '+(bad.length-40)+' 张');
JS
