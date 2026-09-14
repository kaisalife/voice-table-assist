// CellHit.aidl —— 一次「说完了」的解析结果（方案 §3.2）
// 注意：row/column 按 AIDL 契约输出 0-indexed（内部向量库为 1-based，服务端已做 -1 换算）。
package com.vta;

parcelable CellHit {
    int row;                // 0-indexed
    int column;             // 0-indexed
    String value;           // 归一后的字符串，如 "17.84"（中文数字已转阿拉伯）
    String raw;             // 原始 ASR 文本片段（如 "十七点八四"）
    long finalizedAtMs;     // 解析完成时间（epoch ms）
}
