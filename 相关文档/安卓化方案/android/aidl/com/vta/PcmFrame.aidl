// PcmFrame.aidl —— 16kHz float32 单声道 PCM 块（对齐现网 WS 二进制帧）
package com.vta;

parcelable PcmFrame {
    long timestampMs;       // 企业 App 自己写，服务端不解析
    float[] samples;        // 10~100ms 长度（160~1600 样本）；单块上限 4096 样本（256ms）
}
