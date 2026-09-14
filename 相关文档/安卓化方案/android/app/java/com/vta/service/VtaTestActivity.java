package com.vta.service;

import android.app.Activity;
import android.Manifest;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.method.ScrollingMovementMethod;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TableLayout;
import android.widget.TableRow;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * VTA 安卓端测试页（与网关测试前端 index.html 对齐：表格 + 语音录入 + 回填）。
 *
 *   1) 初始化：解包 assets/models → filesDir，启动 native 服务，渲染巡检表
 *   2) 离线自检：跑打包的 16k PCM 全链路（不需要麦克风）——验收主手段
 *   3) 开始录入 / 停止：真实麦克风（AAudio，captureMode=0），实时显示 partial 并回填单元格
 *
 * 坐标：native 返回 0-indexed（行0=第一个行标签，列0=一号），与前端 data-r/data-c 的
 *       「第一行标签=第1行」约定一致（本页 rowOffset/colOffset 均为 0）。
 */
public class VtaTestActivity extends Activity {

    private static final int REQ_MIC = 1001;
    private static final int SILENCE_MS = 300;

    private TextView mLog, mPartial, mAccum;
    private LinearLayout mCellBox;
    private TableLayout mGrid;
    private TextView mGridTitle;
    private android.widget.Spinner mTableSel;
    private Button mBtnInit, mBtnSelftest, mBtnStart, mBtnStop, mBtnImport;
    private final java.util.List<String> mTableNames = new ArrayList<>();
    private String mCurrentTable = "";   // 当前活动表（同一时刻只有一张；用到哪张用哪张的数据）

    private File mDataDir;
    private volatile int mSessionId = 0;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final List<String> mCells = new ArrayList<>();
    private final StringBuilder mFullLog = new StringBuilder();
    private volatile boolean mPolling = false;  // 后台轮询开关
    /** data-r/data-c（1-based，含偏移） → 该单元格 TextView */
    private final Map<String, TextView> mCellViews = new HashMap<>();
    private int mRowCount = 0, mColCount = 0;


    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(buildUi());
        mDataDir = new File(getFilesDir(), "vta");
        append("数据目录: " + mDataDir.getAbsolutePath());
        append("点「1 初始化」开始（首次解包模型约 1~2 分钟）");

        String autorun = getIntent() != null ? getIntent().getStringExtra("autorun") : null;
        if ("selftest".equals(autorun) || "listen".equals(autorun)) {
            append("autorun=" + autorun);
            mHandler.post(() -> doInit(() -> {
                if ("selftest".equals(autorun)) doSelftest();
                else runOnUiThread(this::doStart);
            }));
        }
    }

    // ---------------- UI ----------------
    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(10), dp(12), dp(10), dp(8));

        TextView title = new TextView(this);
        title.setText("VTA 安卓端语音录入测试");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        LinearLayout row1 = new LinearLayout(this);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        mBtnInit = btn("1 初始化");
        mBtnSelftest = btn("2 离线自检");
        row1.addView(mBtnInit); row1.addView(mBtnSelftest);
        root.addView(row1);

        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        mBtnStart = btn("3 开始录入");
        mBtnStop = btn("4 停止并出结果");
        mBtnStop.setEnabled(false);
        row2.addView(mBtnStart); row2.addView(mBtnStop);
        root.addView(row2);

        // ---- 表选择 + 导入（多表共存：导入多张，同一时刻只用一张）----
        LinearLayout rowT = new LinearLayout(this);
        rowT.setOrientation(LinearLayout.HORIZONTAL);
        rowT.setGravity(Gravity.CENTER_VERTICAL);
        mTableSel = new android.widget.Spinner(this);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        slp.setMargins(dp(3), dp(6), dp(3), 0);
        mTableSel.setLayoutParams(slp);
        rowT.addView(mTableSel);
        mBtnImport = btn("导入表");
        rowT.addView(mBtnImport);
        root.addView(rowT);
        mTableSel.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> p, View v, int pos, long id) {
                if (pos >= 0 && pos < mTableNames.size()) onTableSelected(mTableNames.get(pos));
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> p) {}
        });

        // ---- 实时识别 ----
        TextView lb = new TextView(this);
        lb.setText("实时识别：");
        lb.setPadding(0, dp(8), 0, 0);
        root.addView(lb);
        mPartial = new TextView(this);
        mPartial.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        mPartial.setTextColor(Color.parseColor("#1A5FB4"));
        mPartial.setMinHeight(dp(40));
        mPartial.setGravity(Gravity.CENTER_VERTICAL);
        mPartial.setBackgroundColor(Color.parseColor("#F2F6FC"));
        mPartial.setPadding(dp(8), dp(4), dp(8), dp(4));
        root.addView(mPartial);
        mAccum = new TextView(this);
        mAccum.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        mAccum.setTextColor(Color.parseColor("#71717A"));
        mAccum.setPadding(dp(4), dp(2), 0, 0);
        root.addView(mAccum);

        // ---- 巡检表 ----
        mGridTitle = new TextView(this);
        mGridTitle.setText("巡检表（初始化后显示）");
        mGridTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        mGridTitle.setTypeface(Typeface.DEFAULT_BOLD);
        mGridTitle.setPadding(0, dp(10), 0, dp(4));
        root.addView(mGridTitle);

        HorizontalScrollView hsv = new HorizontalScrollView(this);
        mGrid = new TableLayout(this);
        hsv.addView(mGrid);
        root.addView(hsv);

        // ---- 日志 ----
        TextView ll = new TextView(this);
        ll.setText("运行日志：");
        ll.setPadding(0, dp(8), 0, 0);
        root.addView(ll);
        ScrollView sv = new ScrollView(this);
        mLog = new TextView(this);
        mLog.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        mLog.setTypeface(Typeface.MONOSPACE);
        mLog.setMovementMethod(new ScrollingMovementMethod());
        sv.addView(mLog);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(sv, lp);

        mBtnInit.setOnClickListener(v -> doInit(null));
        mBtnSelftest.setOnClickListener(v -> doSelftest());
        mBtnStart.setOnClickListener(v -> doStart());
        mBtnStop.setOnClickListener(v -> doStop());
        mBtnImport.setOnClickListener(v -> showImportDialog());
        return root;
    }

    // ---------------- 多表：列表 / 选择 / 导入 ----------------
    /** 刷新表下拉框（列出全部已导入的表）——所有 UI 操作都在主线程 */
    private void refreshTables() {
        String[] tables = NativeBridge.nativeListTables();
        final List<String> names = new ArrayList<>();
        if (tables != null) {
            for (String t : tables) {
                int bar = t.indexOf('|');
                names.add(bar > 0 ? t.substring(0, bar) : t);
            }
        }
        runOnUiThread(() -> {
            mTableNames.clear();
            mTableNames.addAll(names);
            mTableSel.setAdapter(new android.widget.ArrayAdapter<>(this,
                    android.R.layout.simple_spinner_dropdown_item, mTableNames));
            if (!mTableNames.isEmpty()) {
                int idx = mTableNames.indexOf(mCurrentTable);
                if (idx < 0) idx = 0;
                mCurrentTable = mTableNames.get(idx);
                mTableSel.setSelection(idx);
                renderTable(mCurrentTable);
            }
        });
        append("已导入表：" + (names.isEmpty() ? "（无）" : String.join("、", names)));
    }

    /** 切换当前活动表：渲染该表表格（用到哪张表用哪张表的数据） */
    private void onTableSelected(String name) {
        if (name == null || name.isEmpty() || name.equals(mCurrentTable)) return;
        if (mSessionId > 0) {
            Toast.makeText(this, "录音中不能切表，请先停止", Toast.LENGTH_SHORT).show();
            return;
        }
        mCurrentTable = name;
        append("切换到表：" + name);
        renderTable(name);
    }

    /** 渲染指定表的表格（行标签/列数来自该表的索引） */
    private void renderTable(String name) {
        new Thread(() -> {
            String meta = NativeBridge.nativeTableMeta(name);
            if (meta == null || meta.isEmpty()) {
                post("未取到表结构：" + name);
                return;
            }
            String[] lines = meta.split("\n");
            String[] head = lines[0].split("\\|");
            List<String> rows = new ArrayList<>();
            for (int i = 1; i < lines.length; ++i)
                if (!lines[i].trim().isEmpty()) rows.add(lines[i].trim());
            int cols = head.length > 2 ? Integer.parseInt(head[2].trim()) : 0;
            final String tname = head[0];
            runOnUiThread(() -> renderGrid(tname, rows, cols));
        }, "vta-meta").start();
    }

    /** 导入表弹窗：表名 + 行标签（每行一个）+ 列数 */
    private void showImportDialog() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(8), dp(16), dp(8));

        final android.widget.EditText etName = new android.widget.EditText(this);
        etName.setHint("表名，如：水泵巡检");
        box.addView(etName);

        final android.widget.EditText etRows = new android.widget.EditText(this);
        etRows.setHint("行标签，每行一个，如：\n流量\n扬程\n电流");
        etRows.setMinLines(3);
        etRows.setGravity(Gravity.TOP);
        box.addView(etRows);

        final android.widget.EditText etCols = new android.widget.EditText(this);
        etCols.setHint("列数，如：4");
        etCols.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        box.addView(etCols);

        new android.app.AlertDialog.Builder(this)
                .setTitle("导入表（多表共存，导入后自动切换）")
                .setView(box)
                .setNegativeButton("取消", null)
                .setPositiveButton("导入", (d, w) -> {
                    String name = etName.getText().toString().trim();
                    String colsStr = etCols.getText().toString().trim();
                    List<String> rows = new ArrayList<>();
                    for (String line : etRows.getText().toString().split("\n")) {
                        String s = line.trim();
                        if (!s.isEmpty()) rows.add(s);
                    }
                    int cols = 0;
                    try { cols = Integer.parseInt(colsStr); } catch (NumberFormatException ignored) {}
                    if (name.isEmpty() || rows.isEmpty() || cols <= 0) {
                        Toast.makeText(this, "表名/行标签/列数都要填", Toast.LENGTH_LONG).show();
                        return;
                    }
                    final String fName = name;
                    final int fCols = cols;
                    new Thread(() -> {
                        int rc = NativeBridge.nativeImportTable(fName, rows.toArray(new String[0]), fCols);
                        post("导入表 " + fName + "：" + (rc == 0 ? "成功" : ("失败 rc=" + rc)));
                        if (rc == 0) {
                            mCurrentTable = fName;  // 导入后自动切到新表
                            refreshTables();
                            renderTable(fName);
                        }
                    }, "vta-import").start();
                })
                .show();
    }

    private Button btn(String text) {
        Button b = new Button(this);
        b.setText(text);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(dp(3), dp(5), dp(3), 0);
        b.setLayoutParams(lp);
        return b;
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics());
    }

    private TextView gridCell(String text, boolean header, boolean label) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, header ? 13 : 15);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(dp(10), dp(9), dp(10), dp(9));
        tv.setBackgroundColor(header ? Color.parseColor("#F1F5F9")
                : (label ? Color.parseColor("#FAFAFA") : Color.WHITE));
        if (header || label) tv.setTypeface(Typeface.DEFAULT_BOLD);
        TableRow.LayoutParams lp = new TableRow.LayoutParams();
        lp.setMargins(dp(1), dp(1), 0, 0);
        tv.setLayoutParams(lp);
        // 细边框：用背景色与 margin 模拟（避免引入 res/drawable）
        return tv;
    }

    /** 渲染巡检表：表头「项目 \ 位置 | 1号 | 2号 …」，每行 行标签 + 单元格「—」 */
    private void renderGrid(String name, List<String> rows, int cols) {
        mRowCount = rows.size();
        mColCount = cols;
        mCellViews.clear();
        mGrid.removeAllViews();
        mGridTitle.setText("巡检表 — " + name + "（" + rows.size() + " 行 × " + cols + " 列）");

        TableRow head = new TableRow(this);
        head.addView(gridCell("项目 \\ 位置", true, false));
        for (int c = 1; c <= cols; ++c) head.addView(gridCell(c + "号", true, false));
        mGrid.addView(head);

        for (int i = 0; i < rows.size(); ++i) {
            TableRow tr = new TableRow(this);
            tr.addView(gridCell(rows.get(i), false, true));
            for (int c = 1; c <= cols; ++c) {
                TextView cell = gridCell("—", false, false);
                mCellViews.put(i + 1 + "-" + (c), cell);
                tr.addView(cell);
            }
            mGrid.addView(tr);
        }
        append("表格已渲染：" + name + " " + rows.size() + "行×" + cols + "列");
    }

    /**
     * 把命中结果填进表格：cell 形如 "row|col|value|raw"，row/col 为 1-based
     * （第一个行标签=第1行，一号=第1列，与前端 data-r/data-c 约定一致；偏移均为 0）。
     */
    private void applyCell(final String cell) {
        String[] p = cell.split("\\|");
        if (p.length < 4) return;
        final int r, c;
        try { r = Integer.parseInt(p[0].trim()); c = Integer.parseInt(p[1].trim()); }
        catch (NumberFormatException e) { return; }
        final String value = p[2], raw = p[3];
        final String key = r + "-" + c;  // 1-based 坐标
        runOnUiThread(() -> {
            TextView tv = mCellViews.get(key);
            if (tv != null) {
                tv.setText(value);
                tv.setTextColor(Color.parseColor("#166534"));
                tv.setTypeface(Typeface.DEFAULT_BOLD);
                tv.setBackgroundColor(Color.parseColor("#DCFCE7"));
            }
            mAccum.setText("最近命中：第 " + r + " 行 第 " + c + " 列 = " + value
                    + "（原始“" + raw + "”）");
            mCells.add(key + "|" + value + "|" + raw);
        });
    }

    // ---------------- 步骤 1：初始化 ----------------
    private void doInit(Runnable onDone) {
        mBtnInit.setEnabled(false);
        new Thread(() -> {
            try {
                long t0 = System.currentTimeMillis();
                int rc = NativeBridge.nativeExtractAssets(getAssets(), mDataDir.getAbsolutePath());
                post("解包 assets: rc=" + rc + " (" + (System.currentTimeMillis() - t0) + "ms)");
                int rc2 = NativeBridge.nativeStart(mDataDir.getAbsolutePath());
                post("nativeStart: rc=" + rc2);
                post("health=" + NativeBridge.nativeHealth());

                // 列出全部已导入表 → 下拉选择（多表共存，同一时刻只用一张）
                refreshTables();
                post("初始化完成 ✅ 可「2 离线自检」「3 开始录入」，或用「导入表」加新表");
                if (onDone != null) runOnUiThread(onDone);
            } catch (Throwable e) {
                post("初始化异常: " + e);
            } finally {
                runOnUiThread(() -> mBtnInit.setEnabled(true));
            }
        }, "vta-init").start();
    }

    // ---------------- 步骤 2：离线自检 ----------------
    private void doSelftest() {
        mBtnSelftest.setEnabled(false);
        new Thread(() -> {
            try {
                // 用例①：标准语句（验证整条链路）
                runOneSelftest("test/selftest_16k.f32", "① 标准语句「硬度一号是十七点八四…」");
                // 用例②：同音错字（验证表内读音对齐：TTS 说"水围一号五米"，ASR 常听成"水为/水卫"）
                runOneSelftest("test/homophone_16k.f32", "② 同音纠错「水围一号五米」");
            } catch (Throwable e) {
                post("自检异常: " + e);
            } finally {
                runOnUiThread(() -> mBtnSelftest.setEnabled(true));
            }
        }, "vta-selftest").start();
    }

    /** 跑一个测试 PCM：先确保已解包，再执行全链路并把结果填进表格 */
    private void runOneSelftest(String assetRel, String title) throws Exception {
        File pcm = new File(mDataDir, assetRel);
        if (!pcm.exists()) copyAsset(assetRel, pcm);
        post("—— " + title + " ——");
        long t0 = System.currentTimeMillis();
        String out = NativeBridge.nativeSelftest(pcm.getAbsolutePath(), mDataDir.getAbsolutePath());
        post("耗时 " + (System.currentTimeMillis() - t0) + "ms");
        for (String line : out.split("\n")) {
            if (line.startsWith("cell:")) {
                post("  " + line);
                applyCell(line.substring(5));
            } else if (line.startsWith("partial:")) {
                post("  " + line);
                final String t = line.substring(8);
                runOnUiThread(() -> mPartial.setText(t));
            } else {
                post(line);
            }
        }
    }

    private void copyAsset(String assetPath, File dst) throws Exception {
        dst.getParentFile().mkdirs();
        try (InputStream in = getAssets().open(assetPath);
             FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    // ---------------- 步骤 3/4：真实麦克风 ----------------
    private void doStart() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
            return;
        }
        mCells.clear();
        mPartial.setText("");
        if (mCurrentTable.isEmpty()) {
            Toast.makeText(this, "还没有表，请先「导入表」", Toast.LENGTH_LONG).show();
            return;
        }
        int sid = NativeBridge.nativeOpenSession(mCurrentTable, SILENCE_MS, 0); // captureMode=0 → AAudio
        post("openSession(" + mCurrentTable + ") → " + sid);
        if (sid <= 0) {
            Toast.makeText(this, "开启会话失败: " + sid, Toast.LENGTH_LONG).show();
            return;
        }
        mSessionId = sid;
        mBtnStart.setEnabled(false);
        mBtnStop.setEnabled(true);
        startPolling(sid);
        Toast.makeText(this, "开始说话，停顿后自动回填", Toast.LENGTH_SHORT).show();
    }

    /**
     * 轮询必须在【后台线程】：getState/pollCells 是 JNI 调用，pollCells 内部最多阻塞 250ms，
     * 放在主线程会造成每 120ms 一次的界面卡顿（实测"语音输入卡顿感"的来源）。
     */
    private void startPolling(final int sid) {
        mPolling = true;
        new Thread(() -> {
            while (mPolling && mSessionId == sid) {
                try {
                    String st = NativeBridge.nativeGetState(sid);
                    int bar = st.indexOf('|');
                    if (bar > 0) { final String p = st.substring(bar + 1); postPartial(p); }
                    String[] cells = NativeBridge.nativePollCells(sid, 16);
                    if (cells != null) for (String c : cells) applyCell(c);
                } catch (Throwable ignored) {
                }
                try { Thread.sleep(120); } catch (InterruptedException e) { return; }
            }
        }, "vta-poll").start();
    }

    private void postPartial(final String text) {
        runOnUiThread(() -> mPartial.setText(text));
    }

    private void doStop() {
        final int sid = mSessionId;
        mSessionId = 0;
        mPolling = false;
        mBtnStop.setEnabled(false);
        new Thread(() -> {
            try {
                int rc = NativeBridge.nativeCloseSession(sid);
                post("closeSession → " + rc);
                String[] cells = NativeBridge.nativePollCells(sid, 64);
                if (cells != null) for (String c : cells) { post("cell: " + c); applyCell(c); }
                if (cells == null || cells.length == 0) post("（无解析结果）");
            } catch (Throwable e) {
                post("停止异常: " + e);
            } finally {
                runOnUiThread(() -> mBtnStart.setEnabled(true));
            }
        }, "vta-stop").start();
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        super.onRequestPermissionsResult(code, perms, res);
        if (code == REQ_MIC && res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED) {
            doStart();
        } else if (code == REQ_MIC) {
            post("未授予麦克风权限（可用「2 离线自检」验证链路）");
        }
    }

    // ---------------- 日志 ----------------
    private void post(final String line) { append(line); }

    /** 线程安全：任何线程调用都会回到主线程更新 UI（StringBuilder 非线程安全） */
    private void append(String line) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            final String l = line;
            runOnUiThread(() -> append(l));
            return;
        }
        mFullLog.append(line).append('\n');
        mLog.setText(mFullLog.toString());
    }

    @Override protected void onDestroy() {
        mPolling = false;
        if (mSessionId > 0) NativeBridge.nativeCloseSession(mSessionId);
        super.onDestroy();
    }
}
