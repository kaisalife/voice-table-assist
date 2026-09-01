/* ============================================================================
   selftest/ws_client.js — 极简 WS 客户端，供 selftest.bat 调用。
   依赖：本机安装 Node.js (>=14)；Windows 上 winget install OpenJS.NodeJS.LTS
   npm 无额外依赖，用 Node 自带 ws 不现实；这里采用 ws 包最稳定且通用。
   但用户可能没装 ws，所以脚本先 require('ws')，若不存在降级回退方案：
   用 Node 自带 HTTP 做 TCP 连接……这过于繁琐。因此：
     - 若 require('ws') 失败，脚本打印安装命令并返回退出码 3（SKIP），
       让 bat 上游统一处理"SKIP 不计失败"语义。
   ============================================================================
   用法：
     node ws_client.js ready    <wsURL> <timeoutMs>
        连 WS，等待 "type":"ready"，输出毫秒数到 stdout（纯数字）。
        退出码：0 = 收到 ready；1 = 超时/错误；2 = 主动 CLOSE；3 = 缺 ws 依赖

     node ws_client.js asr      <wsURL> <float32PcmFile> <timeoutMs>
        连 WS → 等 ready → 分片发送 float32 PCM 二进制 → 发 {"type":"stop"}
        → 循环收 60s，看到 type=final 打印 FINAL；看到 type=cells 打印 CELLS 并退出 0。
        退出码：0 = 收到 cells；1 = 出错/无 cells；3 = 缺 ws 依赖
   ============================================================================ */
'use strict';

const fs = require('fs');
const { spawnSync } = require('child_process');

let WebSocketCtor = null;
try {
    WebSocketCtor = require('ws');
} catch (_) {
    try {
        // 尝试在 selftest/ 目录旁边的 node_modules 里找
        const local = require.resolve('ws', { paths: [__dirname, process.cwd()] });
        WebSocketCtor = require(local);
    } catch (__) {
        process.stderr.write(
            '[ws_client] npm package "ws" is required for WebSocket tests.\n' +
            '           Install once:  cd ' + __dirname + ' && npm install ws\n' +
            '           Or global:     npm install -g ws\n' +
            '           Install Node.js first if needed: winget install OpenJS.NodeJS.LTS\n'
        );
        process.exit(3); // 3 => 依赖缺失（SKIP）
    }
}

const cmd = process.argv[2];
const url = process.argv[3];

if (cmd === 'ready') {
    const timeout = parseInt(process.argv[4] || '60000', 10);
    runReady(url, timeout);
} else if (cmd === 'asr') {
    const pcmFile = process.argv[4];
    const timeout = parseInt(process.argv[5] || '90000', 10);
    runAsr(url, pcmFile, timeout);
} else {
    process.stderr.write('usage: ws_client.js {ready|asr} <wsURL> ...\n');
    process.exit(2);
}

function runReady(wsUrl, timeoutMs) {
    const ws = new WebSocketCtor(wsUrl, { handshakeTimeout: timeoutMs });
    const t0 = Date.now();
    let done = false;
    const kill = setTimeout(() => {
        if (done) return;
        done = true;
        try { ws.close(); } catch (_) {}
        process.stderr.write('READY timeout (' + timeoutMs + 'ms)\n');
        process.exit(1);
    }, timeoutMs);

    ws.on('open', () => { /* ok */ });
    ws.on('message', (data) => {
        if (done) return;
        const s = (typeof data === 'string') ? data : data.toString('utf8');
        if (/"type"\s*:\s*"ready"/.test(s)) {
            const ms = Date.now() - t0;
            done = true;
            clearTimeout(kill);
            process.stdout.write(String(ms));
            // 给服务端留时间让 background activate 落定（类似原脚本 sleep 300ms）
            setTimeout(() => { try { ws.close(); } catch(_){} process.exit(0); }, 600);
        }
    });
    ws.on('close', () => {
        if (done) return;
        done = true;
        clearTimeout(kill);
        process.stderr.write('WS closed before ready frame\n');
        process.exit(2);
    });
    ws.on('error', (e) => {
        if (done) return;
        done = true;
        clearTimeout(kill);
        process.stderr.write('WS error: ' + (e && e.message || e) + '\n');
        process.exit(1);
    });
}

function runAsr(wsUrl, pcmFile, timeoutMs) {
    const ws = new WebSocketCtor(wsUrl, { handshakeTimeout: timeoutMs });
    let ready = false;
    let gotFinal = false;
    let gotCells = false;
    let done = false;

    const deadlineT = setTimeout(() => {
        if (done) return;
        done = true;
        try { ws.close(); } catch(_){}
        process.stderr.write('ASR deadline (' + timeoutMs + 'ms) reached; ');
        process.stderr.write(
            (gotCells ? 'CELLS ok' : (gotFinal ? 'final but no cells' : 'no final / cells')) + '\n'
        );
        process.exit(gotCells ? 0 : 1);
    }, timeoutMs);

    ws.on('open', () => { /* ok */ });

    ws.on('message', (data) => {
        const s = (typeof data === 'string') ? data : data.toString('utf8');
        process.stdout.write('RECV> ' + s + '\n');
        if (/"type"\s*:\s*"ready"/.test(s)) {
            if (ready) return;
            ready = true;
            sendPcm();
        }
        if (/"type"\s*:\s*"final"/.test(s)) gotFinal = true;
        if (/"type"\s*:\s*"cells"/.test(s)) {
            gotCells = true;
            // 再多等 500ms 收集剩余帧，然后优雅结束
            setTimeout(() => { try { ws.close(); } catch(_){} finish(); }, 500);
        }
    });

    ws.on('close', () => { if (!done) finish(); });
    ws.on('error', (e) => {
        process.stderr.write('WS error: ' + (e && e.message || e) + '\n');
        finish(1);
    });

    function finish(forcedExit) {
        if (done) return;
        done = true;
        clearTimeout(deadlineT);
        if (gotCells) { process.stdout.write('CELLS\n'); process.exit(0); }
        if (gotFinal) { process.stdout.write('FINAL_NO_CELLS\n'); process.exit(1); }
        process.stdout.write('NO_RESULT\n');
        process.exit(typeof forcedExit === 'number' ? forcedExit : 1);
    }

    function sendPcm() {
        fs.readFile(pcmFile, (err, buf) => {
            if (err) { process.stderr.write('read pcm error: ' + err.message + '\n'); finish(1); return; }
            process.stdout.write('SENDING ' + buf.length + ' bytes float32 PCM ...\n');
            const chunk = 4096; // ~64ms @16k float32
            let off = 0;
            const sendOne = () => {
                if (off >= buf.length) {
                    // 发 stop
                    try { ws.send(JSON.stringify({ type: 'stop' })); }
                    catch(e){ process.stderr.write('send stop err: '+e.message+'\n'); finish(1); }
                    return;
                }
                const end = Math.min(off + chunk, buf.length);
                const seg = buf.slice(off, end);
                try { ws.send(seg, { binary: true }, (e) => {
                    if (e) { process.stderr.write('send err: ' + e.message + '\n'); finish(1); return; }
                    off = end;
                    setTimeout(sendOne, 60); // 模拟流式速率
                }); } catch(e) {
                    process.stderr.write('send throw: ' + e.message + '\n'); finish(1);
                }
            };
            sendOne();
        });
    }
}
