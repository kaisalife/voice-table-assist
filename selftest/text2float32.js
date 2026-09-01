/* ============================================================================
   selftest/text2float32.js — WAV(int16, 16k, mono) → 二进制 float32 PCM 文件
   用法：node text2float32.js <in.wav> <out.f32.pcm>
   严格解析 data chunk；输出 raw bytes（供 ws_client.js asr 节发送）
   ============================================================================ */
'use strict';
const fs = require('fs');
const inFile = process.argv[2];
const outFile = process.argv[3];
if (!inFile || !outFile) { process.stderr.write('usage: text2float32.js <in.wav> <out.f32.pcm>\n'); process.exit(2); }

const bytes = fs.readFileSync(inFile);
// 查 "data" chunk
let off = -1, len = 0;
for (let i = 0; i <= bytes.length - 8; i++) {
    if (bytes[i]===0x64 && bytes[i+1]===0x61 && bytes[i+2]===0x74 && bytes[i+3]===0x61) {
        len = bytes.readUInt32LE(i+4);
        off = i + 8;
        break;
    }
}
if (off < 0 || len <= 0 || off + len > bytes.length) {
    process.stderr.write('wav data chunk not found (off='+off+' len='+len+' total='+bytes.length+')\n');
    process.exit(1);
}
const count = Math.floor(len / 2);
const out = Buffer.alloc(count * 4);
for (let i = 0; i < count; i++) {
    const v = bytes.readInt16LE(off + i*2);
    out.writeFloatLE(v / 32768, i*4, true);
}
fs.writeFileSync(outFile, out);
process.stdout.write('WROTE ' + count + ' samples -> ' + outFile + ' (' + out.length + ' bytes)\n');
process.exit(0);
