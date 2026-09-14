// TableInfo.aidl —— 表注册表条目（方案 §3.2）
package com.vta;

parcelable TableInfo {
    String name;            // 表名（如"锅炉巡检"）
    String key;             // 文件系统安全 key（SanitizeTableKey）
    int rows;
    int columns;
    long updatedAtMs;       // 导入时间（epoch ms；未知为 0）
}
