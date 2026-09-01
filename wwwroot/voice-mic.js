/**
 * VoiceMic — 最小语音录入客户端（纯传输，无任何 UI）。
 *
 * 交互逻辑全部在服务端：文本合并、静默自动提交、RaNER 解析。
 * 前端只负责：开关麦克风、收结果。接入只需三步：
 *
 *   <script src="http://<host>:15232/voice-mic.js"></script>
 *   <button onclick="mic.toggle()">语音录入</button>
 *   <script>
 *     const mic = VoiceMic.create({
 *       base: 'http://<host>:15232',          // 网关地址；省略=当前页面 origin
 *       table: '力学性能',                     // 可选；省略=当前活动表
 *       onResult: (cells, text) => { ... },   // 说完停顿后返回 [{row,column,values}]
 *       onError: (message) => { ... },        // 可选
 *     })
 *   </script>
 *
 * 连接顺序（重要）：点击后先连 WS 再请求麦克风权限——服务端懒加载模型期间
 * 浏览器同时弹权限窗，两件事并行，谁都不空等；加载进度经 onStateChange
 * （state='loading'）通知页面展示。
 */
(function exposeVoiceMic(global) {
  'use strict'

  function toWsUrl(base) {
    const url = new URL('api/speech/asr/stream', base || global.location.origin)
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:'
    return url
  }

  function createVoiceMic(options = {}) {
    const settings = {
      base: options.base || global.location.origin,
      table: options.table || '',
      workletUrl: options.workletUrl || null,       // 缺省 = base 下的 audio-capture-worklet.js
      // 覆盖"点击→可说话"全过程（含懒加载冷启动与麦克风权限弹窗）。
      // 收到服务端 loading 进度会自动顺延，不会误杀慢加载。
      readyTimeoutMs: options.readyTimeoutMs || 30000,
      onResult: typeof options.onResult === 'function' ? options.onResult : () => {},
      onTranscript: typeof options.onTranscript === 'function' ? options.onTranscript : null,
      onLevel: typeof options.onLevel === 'function' ? options.onLevel : null,
      onDiagnostics: typeof options.onDiagnostics === 'function' ? options.onDiagnostics : null,
      onError: typeof options.onError === 'function' ? options.onError : () => {},
      onStateChange: typeof options.onStateChange === 'function' ? options.onStateChange : () => {},
    }

    let enabled = false
    let stopping = false
    let socket = null
    let audioContext = null
    let capture = null
    let gen = 0              // 会话代号：cancel()/hardStop() 后，一切在途异步结果作废
    let readyTimer = null

    function send(obj) {
      if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(obj))
    }

    /** 当前调用是否已被更新的一次 start/cancel 取代 */
    function stale(myGen) { return myGen !== gen }

    async function start() {
      if (enabled) return
      if (!navigator.mediaDevices?.getUserMedia) throw new Error('当前环境不支持麦克风（需 HTTPS 或 localhost）')
      if (!global.AudioWorkletNode) throw new Error('当前环境不支持 AudioWorklet')

      enabled = true
      stopping = false
      const myGen = ++gen                 // 新会话开始：旧会话的一切回调自此失效
      settings.onStateChange({ state: 'connecting' })
      // 看门狗从点击瞬间布防：覆盖权限弹窗（可能没人理）+ 连接 + 懒加载 + ready，
      // 任何一环卡死都能在超时后明确报错退出，绝不再无限转圈。
      armWatchdog(myGen, settings.readyTimeoutMs)
      try {
        connect(myGen)                    // 先连后端：懒加载进度帧与权限弹窗并行
        await prepareAudio(myGen)
        if (stale(myGen)) return          // 等权限/等音频期间被取消或重开 → 本次作废
      } catch (error) {
        if (stale(myGen)) return          // 报错前已被取代：不再打扰新会话
        enabled = false
        clearWatchdog()
        teardown()
        throw error
      }
    }

    /** 错误后的硬停：释放全部资源，用户需重新按按钮开启。 */
    function hardStop() {
      gen++                               // 作废一切在途异步（含未决的 getUserMedia）
      enabled = false
      stopping = false
      clearWatchdog()
      teardown()
      settings.onStateChange({ state: 'stopped' })
    }

    /**
     * 连接阶段取消：与 hardStop 相同的清理路径。
     * 若 getUserMedia 权限弹窗之后才被批准，prepareAudio 内的代号检查会立刻停掉音轨，
     * 不会留下"麦克风亮着但页面以为已关闭"的僵尸采集。
     */
    function cancel() { hardStop() }

    function stop() {
      if (!enabled) return
      enabled = false
      stopping = true
      send({ type: 'stop' })                       // 触发服务端立即提交累积文本
      setTimeout(() => { if (!enabled) { stopping = false; teardown() } }, 800)
    }

    /** 开/关切换：前端按钮的唯一调用点。 */
    function toggle() {
      return enabled ? Promise.resolve(stop()) : start().then(() => true)
    }

    async function prepareAudio(myGen) {
      audioContext = new AudioContext()
      const workletUrl = settings.workletUrl || new URL('audio-capture-worklet.js', settings.base).toString()
      await audioContext.audioWorklet.addModule(workletUrl)
      if (stale(myGen)) { releaseAudio(); return }
      if (audioContext.state === 'suspended') await audioContext.resume()

      const stream = await navigator.mediaDevices.getUserMedia({
        audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
      })
      if (stale(myGen)) {                          // 权限弹窗回来时已被取消 → 立刻还麦
        stream.getTracks().forEach((t) => t.stop())
        releaseAudio()
        return
      }

      const source = audioContext.createMediaStreamSource(stream)
      const node = new AudioWorkletNode(audioContext, 'pcm-capture')
      const silent = audioContext.createGain()
      silent.gain.value = 0

      // Worklet 必须连到输出端才会运行；增益为 0，不外放麦克风声音。
      source.connect(node).connect(silent).connect(audioContext.destination)

      // 采集链路诊断：启动即报一次，之后每 ~25 块（约 1s）随数据流上报
      let blocks = 0
      const report = () => settings.onDiagnostics?.({
        ctxState: audioContext.state, rate: audioContext.sampleRate, blocks,
      })
      report()

      node.port.onmessage = (event) => {
        blocks++
        if (socket?.readyState === WebSocket.OPEN) socket.send(event.data)
        // 输入电平回调：让页面能显示"麦克风是否有声"（无声常见于系统未授权/设备选错）
        if (settings.onLevel) {
          const samples = new Float32Array(event.data)
          let sum = 0
          for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i]
          settings.onLevel(Math.sqrt(sum / samples.length))
        }
        if (blocks % 25 === 0) report()
      }
      capture = { stream, node, silent }
    }

    function armWatchdog(myGen, ms) {
      clearWatchdog()
      readyTimer = setTimeout(() => {
        if (stale(myGen)) return
        settings.onError('连接语音后端超时（可能未授予麦克风权限，或服务正在冷启动），请重试')
        hardStop()
      }, ms)
    }

    function clearWatchdog() {
      if (readyTimer) { clearTimeout(readyTimer); readyTimer = null }
    }

    function connect(myGen) {
      const url = toWsUrl(settings.base)
      if (settings.table) url.searchParams.set('table', settings.table)

      socket = new WebSocket(url.toString())
      socket.binaryType = 'arraybuffer'
      socket.onopen = () => { if (!stale(myGen)) settings.onStateChange({ state: 'connected' }) }
      socket.onclose = () => { if (!stale(myGen)) settings.onStateChange({ state: 'closed' }) }
      socket.onerror = () => { if (!stale(myGen)) settings.onError('语音后端连接失败（可能已有一路语音会话在进行中）') }
      socket.onmessage = (event) => {
        if (stale(myGen)) return
        let msg
        try { msg = JSON.parse(event.data) } catch (_) { return }
        switch (msg.type) {
          case 'loading':
            // 服务端懒加载进度：顺延看门狗（冷启动可能 5~10s），并让页面展示进度
            if (!stale(myGen)) {
              armWatchdog(myGen, 20000)
              settings.onStateChange({ state: 'loading', message: msg.message || '模型加载中...' })
            }
            break
          case 'ready':
            clearWatchdog()
            settings.onStateChange({ state: 'ready' })
            break
          case 'partial':
          case 'final':
            // accumulated：服务端交互会话的当前累计文本（调试显示用，可能为空）
            if (settings.onTranscript) settings.onTranscript(msg.text, !!msg.isFinal, msg.accumulated || '')
            break
          case 'cells':
            settings.onResult(msg.cells || [], msg.text || '')
            break
          case 'error':
            // ACCUM_OVERFLOW：累积超限，服务端已清空。硬停本次会话，用户重新按按钮开启。
            if (msg.code === 'ACCUM_OVERFLOW') {
              settings.onError(msg.message || '语音累计超限已停止')
              hardStop()
              break
            }
            settings.onError(msg.message || '语音处理异常')
            break
        }
      }
    }

    function teardown() {
      releaseAudio()
      try { socket?.close() } catch (_) {}
      socket = null
    }

    function releaseAudio() {
      capture?.node.disconnect()
      capture?.silent.disconnect()
      capture?.stream.getTracks().forEach((track) => track.stop())
      capture = null
      audioContext?.close().catch(() => {})
      audioContext = null
    }

    return {
      get active() { return enabled },
      start, stop, toggle, cancel,
    }
  }

  global.VoiceMic = { create: createVoiceMic }
})(window)
