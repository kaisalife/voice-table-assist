package com.vta.service;

/**
 * Native 桥：与 libvta-service.so 一一对应（jni/service/jni_bridge.cc）。
 * 进程内直调 VtaServiceImpl（普通 APK 无权 addService 到 servicemanager）。
 */
public final class NativeBridge {
    static {
        System.loadLibrary("vta-service");
    }

    private NativeBridge() {}

    /** 解包 assets/models 到 filesDir（幂等）。0=成功 */
    public static native int nativeExtractAssets(android.content.res.AssetManager mgr, String outDir);

    /** 初始化服务。0=成功，-2=Init失败，-3=异常 */
    public static native int nativeStart(String dataDir);

    /** "ok" / "loading:..." / "error:..." / "not_started" */
    public static native String nativeHealth();

    /** 每项 "name|key|rows|cols" */
    public static native String[] nativeListTables();

    /** 表结构：首行 "name|rows|cols"，随后每行一个行标签（供测试页渲染表格） */
    public static native String nativeTableMeta(String tableName);

    /** 导入表（多表共存）：0=成功，-4=失败 */
    public static native int nativeImportTable(String name, String[] rowLabels, int columnCount);

    /** 离线自检（打包的 16k f32 PCM）：多行 "ok=..\\nfinal=..\\ncells=..\\ncell:row|col|value|raw" */
    public static native String nativeSelftest(String pcmPath, String dataDir);

    /** >0=sessionId；409=冲突；-2=参数非法；-3=未就绪 */
    public static native int nativeOpenSession(String tableName, int silenceMs, int captureMode);

    /** "phase|partial" */
    public static native String nativeGetState(int sessionId);

    /** 每项 "row|col|value|raw"（row/col 为 1-based：第一个行标签=第1行，与测试页表格一致） */
    public static native String[] nativePollCells(int sessionId, int maxN);

    public static native int nativeCloseSession(int sessionId);

    public static native int nativeSetDenoise(boolean enabled);

    public static native int nativeVersion();
}
