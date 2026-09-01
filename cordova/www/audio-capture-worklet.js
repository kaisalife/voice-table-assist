/** 将麦克风音频降采样为 16kHz、float32、单声道 PCM。 */
class PcmCaptureProcessor extends AudioWorkletProcessor {
  constructor() {
    super()
    this.targetRate = 16000
    this.chunkSamples = 640 // 每块约 40ms；更连续，避免断断续续。
    this.chunk = []
    this.position = 0
  }

  process(inputs) {
    const input = inputs[0]?.[0]
    if (!input) return true

    const ratio = sampleRate / this.targetRate
    while (this.position < input.length) {
      const value = Math.max(-1, Math.min(1, input[Math.floor(this.position)] || 0))
      this.chunk.push(value)
      this.position += ratio

      if (this.chunk.length >= this.chunkSamples) {
        const pcm = new Float32Array(this.chunk.splice(0, this.chunkSamples))
        this.port.postMessage(pcm.buffer, [pcm.buffer])
      }
    }
    this.position -= input.length
    return true
  }
}

registerProcessor('pcm-capture', PcmCaptureProcessor)

