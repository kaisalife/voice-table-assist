// SessionState.aidl —— getState(sessionId) 轮询返回（方案 §3.2/§3.4）
package com.vta;

parcelable SessionState {
    int sessionId;
    String tableName;       // 表名（如"锅炉巡检"）
    String phase;           // "idle" / "listening" / "endpointing" / "closed"
    String partial;         // 当前 partial 文本（已过 HR 同音纠正 + 领域纠错）
    long lastPartialMs;     // 最近一次 partial 更新时间（epoch ms）
}
