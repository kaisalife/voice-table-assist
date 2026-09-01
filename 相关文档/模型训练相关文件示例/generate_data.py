"""
训练数据生成器 v7 —— 生产环境检测表格（6×6）+ 语音巡检特训
核心模式：行标签(SUB) + 列号(OBJ) + 中文数值 (VAL)
模板来源: ../../moban.txt（外径/内径/表面光洁度/硬度/直线度/圆度，列 1-6）
标签体系：O, B-SUB, I-SUB, B-OBJ, I-OBJ, B-VAL, I-VAL
"""
import json
import random
import os
from itertools import combinations

# ==================== 中文数字工具 ====================

CN_DIGITS = ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九"]

def num_to_cn(n):
    """整数转中文数字（0-9999）"""
    if n == 0:
        return "零"
    if n < 10:
        return CN_DIGITS[n]
    if n < 20:
        return "十" + (CN_DIGITS[n % 10] if n % 10 != 0 else "")
    if n < 100:
        tens = n // 10
        ones = n % 10
        return CN_DIGITS[tens] + "十" + (CN_DIGITS[ones] if ones != 0 else "")
    if n < 1000:
        hundreds = n // 100
        rest = n % 100
        s = CN_DIGITS[hundreds] + "百"
        if rest == 0:
            return s
        if rest < 10:
            return s + "零" + CN_DIGITS[rest]
        return s + num_to_cn(rest)
    if n < 10000:
        thousands = n // 1000
        rest = n % 1000
        s = CN_DIGITS[thousands] + "千"
        if rest == 0:
            return s
        if rest < 100:
            s += "零"
        return s + num_to_cn(rest)
    return str(n)

def decimal_to_cn(n):
    """将数字转为中文，如 17.84 -> 十七点八四"""
    if n < 0:
        return "负" + decimal_to_cn(-n)
    int_part = int(n)
    frac_part = round(n - int_part, 3)
    if frac_part == 0:
        return num_to_cn(int_part)
    frac_str = f"{frac_part:.2f}"[2:]
    cn_int = num_to_cn(int_part) if int_part > 0 else "零"
    cn_frac = "点" + "".join(CN_DIGITS[int(d)] for d in frac_str if d.isdigit())
    return cn_int + cn_frac


# ==================== 数据定义（生产模板） ====================

# 行标签（来自 moban.txt 第3-8列），作为 SUB
ROW_LABELS = ["外径", "内径", "表面光洁度", "硬度", "直线度", "圆度"]

# 序号行号（纯中文），作为 SUB 变体 —— 序号三
ROW_IDS_XUHAO  = [f"序号{CN_DIGITS[i]}" for i in range(1, 7)]    # 序号一..序号六
ROW_IDS_ALL = ROW_IDS_XUHAO

# 列 1-6，作为 OBJ（中文数字表达）
COL_IDS = [f"{num_to_cn(i)}号" for i in range(1, 7)]
COL_IDS_WITH_LIE = [f"{num_to_cn(i)}号列" for i in range(1, 7)]

# 列号口语化（第X个），作为 OBJ 变体 —— 第一个
COL_IDS_DIGE   = [f"第{CN_DIGITS[i]}个" for i in range(1, 7)]    # 第一个..第六个
COL_IDS_DIGE_ALL = COL_IDS_DIGE

# 巡检场景前缀
INSPECTION_PREFIXES = [
    "巡检登记，",
    "巡检登记",
    "登记，",
    "",
]

CONNECTORS = ["填", "是", "为", "等于", "约", "达到", "写", "输入", "改成", "设为"]


# ==================== 多样化数值生成 ====================

def gen_value():
    """生成中文数值：贴合实际生产检测值，范围 0-100，以两位小数为主"""
    vtype = random.random()
    if vtype < 0.80:
        # 两位小数：0.00-99.99，如五点七四
        val = round(random.uniform(0.01, 99.99), 2)
        return decimal_to_cn(val)
    elif vtype < 0.90:
        # 一位小数：0.0-99.9，如五点五
        val = round(random.uniform(0.1, 99.9), 1)
        return decimal_to_cn(val)
    else:
        # 整数：0-100，如五十六
        val = random.randint(0, 100)
        return decimal_to_cn(val)

# 生成大量值池（值变体多，减少去重损失，确保组合覆盖）
VALUE_POOL = [gen_value() for _ in range(8000)]

def gen_whole_zero_value():
    """整数值 + 点零：如 十一点零 / 五点零。口语中常把整数读成 X点零"""
    v = random.randint(1, 100)
    return num_to_cn(v) + "点零"

def pick_val():
    return random.choice(VALUE_POOL)

def pick_val_with_zero():
    """混合普通值和 X点零 值"""
    if random.random() < 0.30:
        return gen_whole_zero_value()
    return pick_val()

def pick_vals(n):
    """一次取 n 个不同的值"""
    return random.sample(VALUE_POOL, min(n, len(VALUE_POOL)))


# ==================== BIO 工具 ====================

def chars_to_bio(segments):
    result = []
    for text, label_type in segments:
        if label_type == "O":
            for ch in text:
                result.append((ch, "O"))
        else:
            for i, ch in enumerate(text):
                tag = f"B-{label_type}" if i == 0 else f"I-{label_type}"
                result.append((ch, tag))
    return result


# ==================== 巡检表格场景模板 ====================

def inspection_single_full():
    """
    单单元格全排列 + 多样式变体。
    对每一个 (行标签, 列号) 组合，穷举多种表达：
      前置/后缀、有无 X号列、有无标点、有无连接词、列前/行前、X号/第X号
    """
    samples = []
    prefix_variants = ["巡检登记，", "巡检登记", "登记，", "登记", ""]
    suffix_variants = ["", "。", "好的", "收到"]

    for row_text in ROW_LABELS:            # 6 行
        for col_text in COL_IDS:           # 6 列
            col_lie = f"{col_text}列"
            # 每种组合用 6 个不同值，保证值多样性且避免重复去重挤掉组合
            for val in pick_vals(6):
                # 变体 1-4：行前，带不带"列"字，带不带连接词
                samples.append(chars_to_bio([
                    (random.choice(prefix_variants), "O"), (row_text, "SUB"), ("，", "O"),
                    (col_lie, "OBJ"), ("，", "O"), (val, "VAL"), (random.choice(suffix_variants), "O"),
                ]))
                samples.append(chars_to_bio([
                    (random.choice(prefix_variants), "O"), (row_text, "SUB"), ("，", "O"),
                    (col_text, "OBJ"), (random.choice(CONNECTORS), "O"), (val, "VAL"),
                ]))
                samples.append(chars_to_bio([
                    (random.choice(prefix_variants), "O"), (row_text, "SUB"),
                    (col_lie, "OBJ"), (val, "VAL"),
                ]))
                samples.append(chars_to_bio([
                    (random.choice(prefix_variants), "O"), (row_text, "SUB"),
                    (col_text, "OBJ"), (random.choice(CONNECTORS), "O"), (val, "VAL"),
                ]))
                # 变体 5-6：列前（列号先说 → 列为主体 SUB；行标签后说 → 行为客体 OBJ）
                samples.append(chars_to_bio([
                    (random.choice(prefix_variants), "O"), (col_lie, "SUB"), ("，", "O"),
                    (row_text, "OBJ"), ("，", "O"), (val, "VAL"),
                ]))
                samples.append(chars_to_bio([
                    (random.choice(prefix_variants), "O"), (col_lie, "SUB"),
                    (row_text, "OBJ"), (random.choice(CONNECTORS), "O"), (val, "VAL"),
                ]))
                # 变体 7：第X号
                samples.append(chars_to_bio([
                    (row_text, "SUB"), (f"第{col_text}", "OBJ"), (val, "VAL"),
                ]))
    return samples


def inspection_same_row():
    """同行多列"""
    samples = []
    for row_text in ROW_LABELS:
        for n_cols in [2, 3, 4]:
            for _ in range(4):
                cols = random.sample(COL_IDS, min(n_cols, len(COL_IDS)))
                cols.sort(key=lambda x: COL_IDS.index(x))
                prefix = random.choice(INSPECTION_PREFIXES)

                segments = [(prefix, "O"), (row_text, "SUB")]
                for ci, col_text in enumerate(cols):
                    if ci > 0 and random.random() < 0.5:
                        segments.append(("，", "O"))
                    segments.append((col_text, "OBJ"))
                    segments.append((pick_val(), "VAL"))
                samples.append(chars_to_bio(segments))
    return samples


def inspection_cross_row():
    """跨行"""
    samples = []
    for _ in range(300):
        n_rows = random.randint(2, 4)
        rows = random.sample(ROW_LABELS, n_rows)
        rows.sort(key=lambda x: ROW_LABELS.index(x))
        prefix = random.choice(INSPECTION_PREFIXES)

        segments = [(prefix, "O")]
        for ri, row_text in enumerate(rows):
            if ri > 0 and random.random() < 0.6:
                segments.append(("，", "O"))
            segments.append((row_text, "SUB"))
            n_cols = random.randint(1, 2)
            cols = random.sample(COL_IDS, n_cols)
            cols.sort(key=lambda x: COL_IDS.index(x))
            for ci, col_text in enumerate(cols):
                if ci > 0 and random.random() < 0.3:
                    segments.append(("，", "O"))
                segments.append((col_text, "OBJ"))
                segments.append((pick_val(), "VAL"))
        samples.append(chars_to_bio(segments))
    return samples


def inspection_with_connectors():
    """巡检场景 + 连接词：外径，一号填VALUE / 外径，一号是VALUE 等"""
    samples = []

    for _ in range(300):
        n_rows = random.randint(1, 3)
        rows = random.sample(ROW_LABELS, n_rows)
        rows.sort(key=lambda x: ROW_LABELS.index(x))
        prefix = random.choice(INSPECTION_PREFIXES)

        segments = [(prefix, "O")]
        for ri, row_text in enumerate(rows):
            if ri > 0:
                segments.append(("，", "O"))
            segments.append((row_text, "SUB"))

            n_cols = random.randint(1, 3)
            cols = random.sample(COL_IDS, n_cols)
            cols.sort(key=lambda x: COL_IDS.index(x))
            for ci, col_text in enumerate(cols):
                if ci > 0 and random.random() < 0.4:
                    segments.append(("，", "O"))
                segments.append((col_text, "OBJ"))
                segments.append((random.choice(CONNECTORS), "O"))
                segments.append((pick_val(), "VAL"))

        samples.append(chars_to_bio(segments))

    # 无标点带连接词版本
    for _ in range(200):
        n_rows = random.randint(1, 3)
        rows = random.sample(ROW_LABELS, n_rows)
        rows.sort(key=lambda x: ROW_LABELS.index(x))

        segments = []
        for ri, row_text in enumerate(rows):
            segments.append((row_text, "SUB"))
            n_cols = random.randint(1, 2)
            cols = random.sample(COL_IDS, n_cols)
            cols.sort(key=lambda x: COL_IDS.index(x))
            for ci, col_text in enumerate(cols):
                segments.append((col_text, "OBJ"))
                segments.append((random.choice(CONNECTORS), "O"))
                segments.append((pick_val(), "VAL"))

        samples.append(chars_to_bio(segments))

    return samples


def inspection_long():
    """巡检长序列：4-6 行，每行 2-4 列，无标点/有标点混合，逼近 128 字符"""
    samples = []
    for _ in range(300):
        n_rows = random.randint(4, 6)
        rows = random.sample(ROW_LABELS, min(n_rows, len(ROW_LABELS)))
        rows.sort(key=lambda x: ROW_LABELS.index(x))

        use_punct = random.random() < 0.5
        prefix = random.choice(INSPECTION_PREFIXES)

        segments = [(prefix, "O")]
        for ri, row_text in enumerate(rows):
            if ri > 0:
                if use_punct and random.random() < 0.7:
                    segments.append(("，", "O"))
            segments.append((row_text, "SUB"))

            n_cols = random.randint(2, 4)
            cols = random.sample(COL_IDS, min(n_cols, len(COL_IDS)))
            cols.sort(key=lambda x: COL_IDS.index(x))
            for ci, col_text in enumerate(cols):
                if ci > 0 and use_punct and random.random() < 0.4:
                    segments.append(("，", "O"))
                segments.append((col_text, "OBJ"))
                segments.append((pick_val(), "VAL"))

        text = "".join(ch for ch, _ in segments)
        if 40 <= len(text) <= 130:
            samples.append(chars_to_bio(segments))

    return samples


def inspection_single_sub_multi_col():
    """
    单主体多客体（确定性全排列）：
    同一行标签(SUB) 下，穷举任意 2~6 个列号(OBJ) 的组合，每列各带一个值(VAL)。
    覆盖 C(6,2)+C(6,3)+C(6,4)+C(6,5)+C(6,6)=56 种列组合 × 多种表达。
    """
    samples = []
    prefix_variants = ["巡检登记，", "巡检登记", "登记，", "登记", ""]

    for row_text in ROW_LABELS:                      # 6 行
        for n_cols in range(2, 7):                    # 2~6 列
            for cols in combinations(COL_IDS, n_cols):  # 56 种列组合
                # 6 客体(满列) 加练：每个组合生成更多值组合，强化全覆盖记法
                rounds = 10 if n_cols == 6 else 3
                for vals in [pick_vals(n_cols) for _ in range(rounds)]:
                    # 变体1：带连接词
                    segments = [(row_text, "SUB")]
                    for ci, (col, val) in enumerate(zip(cols, vals)):
                        if ci > 0 and random.random() < 0.4:
                            segments.append(("，", "O"))
                        segments.append((col, "OBJ"))
                        segments.append((random.choice(CONNECTORS), "O"))
                        segments.append((val, "VAL"))
                    samples.append(chars_to_bio(segments))

                    # 变体2：无连接词 + 前缀
                    segments = [(random.choice(prefix_variants), "O"), (row_text, "SUB")]
                    for ci, (col, val) in enumerate(zip(cols, vals)):
                        if ci > 0 and random.random() < 0.3:
                            segments.append(("，", "O"))
                        segments.append((col, "OBJ"))
                        segments.append((val, "VAL"))
                    samples.append(chars_to_bio(segments))

                    # 变体3：行前 + 强制列间标点 + 连接词（与变体1类似，着重强分隔）
                    segments = [(row_text, "SUB")]
                    for ci, (col, val) in enumerate(zip(cols, vals)):
                        if ci > 0:
                            segments.append(("，", "O"))
                        segments.append((col, "OBJ"))
                        segments.append((random.choice(CONNECTORS), "O"))
                        segments.append((val, "VAL"))
                    samples.append(chars_to_bio(segments))
    return samples


def inspection_col_first():
    """
    列前多行（列主体统摄多行客体）：
    规则：先出现的列号 = SUB；行标签与紧跟其后的值绑一起 = OBJ/VAL，都归当前列主体。
          出现新列号即成为新主体，其后的行-值对归新主体（客体归属跟随语序）。
    单列主体全排列：1 个列 × 2~6 个行（56 组）带全部行组合；并列前多列主体并存。
    """
    samples = []
    prefixes = ["巡检登记，", "巡检登记", "登记，", "登记", ""]
    col_names = COL_IDS_WITH_LIE          # 一号列..六号列

    # 单列主体 + 多行客体（全排列行组合）
    for col_text in col_names:
        for n_rows in range(2, 7):
            for rows in combinations(ROW_LABELS, n_rows):
                for _ in range(2):
                    vals = pick_vals(n_rows)
                    segments = [(random.choice(prefixes), "O"), (col_text, "SUB")]
                    for ci, (row, val) in enumerate(zip(rows, vals)):
                        if ci > 0 and random.random() < 0.4:
                            segments.append(("，" if random.random() < 0.5 else "", "O"))
                        segments.append((row, "OBJ"))
                        if random.random() < 0.5:
                            segments.append((random.choice(CONNECTORS), "O"))
                        segments.append((val, "VAL"))
                    samples.append(chars_to_bio(segments))

    # 多列主体并存（列前 · 每个列主体各配 1~3 行客体）
    for n_cols in range(2, 4):
        for cols in combinations(col_names, n_cols):
            for _ in range(6):
                segments = [(random.choice(prefixes), "O")]
                for ci, col_text in enumerate(cols):
                    if ci > 0 and random.random() < 0.6:
                        segments.append(("，", "O"))
                    segments.append((col_text, "SUB"))
                    for row in random.sample(ROW_LABELS, random.randint(1, 3)):
                        segments.append((row, "OBJ"))
                        if random.random() < 0.5:
                            segments.append((random.choice(CONNECTORS), "O"))
                        segments.append((pick_val(), "VAL"))
                samples.append(chars_to_bio(segments))
    return samples


def multi_sub_multi_obj():
    """
    多主体多客体：多个行标签(SUB)，每个行标签各配多个列号(OBJ)带值(VAL)。
    覆盖 2~3 个主体 × 主体组合 × 每主体 2~3 列，长度控制在 128 字符内。
    """
    samples = []
    prefix_variants = ["巡检登记，", "巡检登记", "登记，", "登记", ""]

    for n_sub in [2, 3]:                          # 2~3 个主体
        for rows in combinations(ROW_LABELS, n_sub):
            for _ in range(4):                    # 每组合多种变体
                col_plan = [random.sample(COL_IDS, random.randint(2, 3)) for _ in rows]
                val_plan = [pick_vals(len(cs)) for cs in col_plan]
                segments = [(random.choice(prefix_variants), "O")]
                for ri, (row, cols) in enumerate(zip(rows, col_plan)):
                    if ri > 0:
                        segments.append(("，", "O"))
                    segments.append((row, "SUB"))
                    for ci, (col, val) in enumerate(zip(cols, val_plan[ri])):
                        if ci > 0 and random.random() < 0.4:
                            segments.append(("，", "O"))
                        segments.append((col, "OBJ"))
                        if random.random() < 0.5:
                            segments.append((random.choice(CONNECTORS), "O"))
                        segments.append((val, "VAL"))
                samples.append(chars_to_bio(segments))
    return samples


def inspection_ordinal_row():
    """
    序号行号为主体(SUB) + 列号为客体(OBJ) + 数值(VAL) 特训。
    场景覆盖：逗号/助词/无标点/中断逗号/前后缀 —— 尽可能多组合。
    参考 提示词.md 示例 3/4：序号一，二号列，十七点八四
    """
    samples = []
    prefixes = ["巡检登记，", "巡检登记", "登记，", "登记", ""]
    suffixes = ["", "。", "好的", "收到"]

    # 所有列号表达池（一号/第一个/第2个/一号列 ...）
    all_cols = COL_IDS + COL_IDS_WITH_LIE + COL_IDS_DIGE

    # ============ 单客体：序号 + 列号 + 值，穷举多种标点/助词/前后缀组合 ============
    for row_id in ROW_IDS_ALL:                     # 序号一..序号6
        for col_text in all_cols:                  # 24 种列号表达
            for _ in range(5):                     # 5 组不同值
                v = pick_val_with_zero()
                c = random.choice(CONNECTORS)
                p = random.choice(prefixes)
                s = random.choice(suffixes)

                # 1. 逗号分隔：SUB，OBJ，VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"), ("，", "O"),
                                             (col_text, "OBJ"), ("，", "O"), (v, "VAL"), (s, "O")]))
                # 2. 逗号+助词：SUB，OBJ 助词 VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"), ("，", "O"),
                                             (col_text, "OBJ"), (c, "O"), (v, "VAL"), (s, "O")]))
                # 3. 无逗号+助词：SUB OBJ 助词 VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"),
                                             (col_text, "OBJ"), (c, "O"), (v, "VAL"), (s, "O")]))
                # 4. 无逗号紧凑：SUB OBJ VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"),
                                             (col_text, "OBJ"), (v, "VAL"), (s, "O")]))
                # 5. SUB后逗号+OBJ紧凑：SUB，OBJVAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"), ("，", "O"),
                                             (col_text, "OBJ"), (v, "VAL"), (s, "O")]))
                # 6. OBJ后逗号+值：SUB OBJ，VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"),
                                             (col_text, "OBJ"), ("，", "O"), (v, "VAL"), (s, "O")]))
                # 7. 助词后逗号：SUB OBJ 助词，VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"),
                                             (col_text, "OBJ"), (c, "O"), ("，", "O"), (v, "VAL"), (s, "O")]))
                # 8. 全逗号+助词：SUB，OBJ，助词，VAL
                samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"), ("，", "O"),
                                             (col_text, "OBJ"), ("，", "O"), (c, "O"), ("，", "O"), (v, "VAL"), (s, "O")]))
                # 9. 无前缀+有后缀：SUB OBJ VAL SUFFIX
                samples.append(chars_to_bio([(row_id, "SUB"), (col_text, "OBJ"), (v, "VAL"), (s, "O")]))
                # 10. 有前缀+无后缀：PREFIX SUB OBJ 助词 VAL
                if p:
                    samples.append(chars_to_bio([(p, "O"), (row_id, "SUB"),
                                                 (col_text, "OBJ"), (c, "O"), (v, "VAL")]))
                # 11. 无前缀无后缀紧凑：SUB OBJ VAL（纯三连）
                samples.append(chars_to_bio([(row_id, "SUB"), (col_text, "OBJ"), (v, "VAL")]))

    # ============ 单主体 + 多客体（同序号下列号变体+逗号位置变化） ============
    for _ in range(1500):
        row_id = random.choice(ROW_IDS_ALL)
        n_cols = random.randint(2, 4)
        cols = random.sample(all_cols, min(n_cols, len(all_cols)))
        p = random.choice(prefixes)
        s = random.choice(suffixes) if random.random() < 0.3 else ""

        # 随机选择逗号策略：全部逗号 / 无逗号 / 中断逗号（部分有部分无）
        comma_mode = random.choice(["all", "none", "intermittent"])
        # 随机选择连接词策略：全部 / 无 / 中断
        conn_mode = random.choice(["all", "none", "intermittent"])

        segments = [(p, "O"), (row_id, "SUB")]
        for ci, col_text in enumerate(cols):
            v = pick_val_with_zero()
            c = random.choice(CONNECTORS)

            # 逗号策略
            if comma_mode == "all" and ci > 0:
                segments.append(("，", "O"))
            elif comma_mode == "intermittent" and ci > 0 and random.random() < 0.5:
                segments.append(("，", "O"))

            segments.append((col_text, "OBJ"))

            # 连接词策略
            if conn_mode == "all":
                segments.append((c, "O"))
            elif conn_mode == "intermittent" and random.random() < 0.5:
                segments.append((c, "O"))

            segments.append((v, "VAL"))

        if s:
            segments.append((s, "O"))
        samples.append(chars_to_bio(segments))

    # ============ 多序号主体 + 各配列客体 ============
    for _ in range(1000):
        n_rows = random.randint(2, 4)
        row_ids = random.sample(ROW_IDS_ALL, min(n_rows, len(ROW_IDS_ALL)))
        p = random.choice(prefixes)
        segments = [(p, "O")]
        for ri, row_id in enumerate(row_ids):
            if ri > 0 and random.random() < 0.6:
                segments.append(("，", "O"))
            segments.append((row_id, "SUB"))
            n_cols = random.randint(1, 2)
            comma_mode = random.choice(["all", "none", "intermittent"])
            for ci, col_text in enumerate(random.sample(all_cols, n_cols)):
                v = pick_val_with_zero()
                c = random.choice(CONNECTORS)
                if comma_mode == "all" and ci > 0:
                    segments.append(("，", "O"))
                elif comma_mode == "intermittent" and ci > 0 and random.random() < 0.5:
                    segments.append(("，", "O"))
                segments.append((col_text, "OBJ"))
                if random.random() < 0.5:
                    segments.append((c, "O"))
                segments.append((v, "VAL"))
        samples.append(chars_to_bio(segments))

    return samples


def inspection_meas_batch():
    """
    【主要类型】行标签为主体(SUB) + 多个"测量值X"为客体(OBJ) + 各带数值(VAL)
    模式：巡检登记，硬度，测量值一是五十点零，测量值二是四十九点九九，...
    覆盖所有 6 个行标签 × 1~6 个测量值，多种逗号/连接词/前后缀组合。
    """
    samples = []
    MEAS_IDS = [f"测量值{CN_DIGITS[i]}" for i in range(1, 7)]  # 测量值一~六

    for row_text in ROW_LABELS:  # 外径, 内径, 表面光洁度, 硬度, 直线度, 圆度
        for n_meas in range(1, 7):  # 1~6 个测量值
            meas_list = MEAS_IDS[:n_meas]

            # 值越多越重要，加练更多
            base_rounds = 30 if n_meas == 6 else (20 if n_meas >= 4 else 10)
            for _ in range(base_rounds):
                vals = pick_vals(n_meas)
                p = random.choice(INSPECTION_PREFIXES)
                s = random.choice(["", "。", "好的", "收到"])
                c = random.choice(CONNECTORS)

                # 变体1: 逗号分隔 + 连接词（是/填/为...）
                segs = [(p, "O"), (row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    if i > 0 or random.random() < 0.5:
                        segs.append(("，", "O"))
                    segs.append((meas, "OBJ"))
                    segs.append((c, "O"))
                    segs.append((val, "VAL"))
                if s: segs.append((s, "O"))
                samples.append(chars_to_bio(segs))

                # 变体2: 逗号分隔 + 无连接词（测量值一，五十点零）
                segs = [(p, "O"), (row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    if i > 0 or random.random() < 0.5:
                        segs.append(("，", "O"))
                    segs.append((meas, "OBJ"))
                    segs.append(("，", "O"))
                    segs.append((val, "VAL"))
                if s: segs.append((s, "O"))
                samples.append(chars_to_bio(segs))

                # 变体3: 逗号+连接词 混合（测量值一是五十点零，测量值二，四十九点九九）
                segs = [(p, "O"), (row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    if i > 0 or random.random() < 0.5:
                        segs.append(("，", "O"))
                    segs.append((meas, "OBJ"))
                    # 有的带连接词，有的纯逗号
                    if random.random() < 0.5:
                        segs.append((random.choice(CONNECTORS), "O"))
                    else:
                        segs.append(("，", "O"))
                    segs.append((val, "VAL"))
                if s: segs.append((s, "O"))
                samples.append(chars_to_bio(segs))

                # 变体4: 无逗号 + 连接词（硬度测量值一是五十点零测量值二是四十九点九九）
                segs = [(p, "O"), (row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    segs.append((meas, "OBJ"))
                    segs.append((c, "O"))
                    segs.append((val, "VAL"))
                if s: segs.append((s, "O"))
                samples.append(chars_to_bio(segs))

                # 变体5: 无逗号 + 无连接词（硬度测量值一五十点零测量值二四十九点九九）
                segs = [(p, "O"), (row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    segs.append((meas, "OBJ"))
                    segs.append((val, "VAL"))
                if s: segs.append((s, "O"))
                samples.append(chars_to_bio(segs))

                # 变体6: 无前缀无后缀紧凑版
                segs = [(row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    if i > 0 and random.random() < 0.5:
                        segs.append(("，", "O"))
                    segs.append((meas, "OBJ"))
                    if random.random() < 0.5:
                        segs.append((random.choice(CONNECTORS), "O"))
                    segs.append((val, "VAL"))
                samples.append(chars_to_bio(segs))

                # 变体7: 有前缀无后缀
                segs = [(p, "O"), (row_text, "SUB")]
                for i, (meas, val) in enumerate(zip(meas_list, vals)):
                    if i > 0:
                        segs.append(("，", "O"))
                    segs.append((meas, "OBJ"))
                    if random.random() < 0.5:
                        segs.append((random.choice(CONNECTORS), "O"))
                    segs.append((val, "VAL"))
                samples.append(chars_to_bio(segs))

    return samples


def generalized_random(n=2500):
    """
    随机化组成：随机抽取若干个 (行, 列) 三元组，随机表达式拼接成句。
    表达方式、标点、连接词、前后缀、列前/行前 全部随机，覆盖无限变体。
    行标签(RowLabel) + 序号行号(序号X) 均作为 SUB 候选。
    """
    samples = []
    prefix_variants = ["巡检登记，", "巡检登记", "登记，", "登记", ""]
    all_cols = COL_IDS + COL_IDS_WITH_LIE + COL_IDS_DIGE

    for _ in range(n):
        # 随机抽取不重复的 (行, 列) 组合
        used = set()
        triples = []
        # 50% 概率用序号做主体，50% 用行标签
        use_xuhao = random.random() < 0.5
        row_pool = ROW_IDS_ALL if use_xuhao else ROW_LABELS
        col_pool = all_cols if use_xuhao else COL_IDS  # 序号场景下用更丰富的列池

        for _ in range(random.randint(1, 6)):
            row = random.choice(row_pool)
            col = random.choice(col_pool)
            key = (row, col)
            if key in used:
                continue
            used.add(key)
            triples.append((row, col, pick_val_with_zero() if use_xuhao else pick_val()))
        if not triples:
            continue

        segments = [(random.choice(prefix_variants), "O")]
        for i, (row, col, val) in enumerate(triples):
            if i > 0 and random.random() < 0.6:
                segments.append(("，", "O"))

            # 行前 或 列前（序号场景只用行前，因为序号是行号概念）
            if use_xuhao or random.random() < 0.75:
                segments.append((row, "SUB"))
                col_ent = f"{col}列" if (not use_xuhao and random.random() < 0.25) else col
                if random.random() < 0.5:
                    segments.append((col_ent, "OBJ"))
                else:
                    segments.append((f"第{col}", "OBJ") if not use_xuhao else (col_ent, "OBJ"))
                if random.random() < 0.5:
                    segments.append((random.choice(CONNECTORS), "O"))
                segments.append((val, "VAL"))
            else:
                col_ent = f"{col}列" if random.random() < 0.25 else col
                segments.append((col_ent, "SUB"))     # 列前 → 列为主体 SUB
                segments.append(("，" if random.random() < 0.3 else "", "O"))
                segments.append((row, "OBJ"))          # 行后 → 行为客体 OBJ
                if random.random() < 0.5:
                    segments.append((random.choice(CONNECTORS), "O"))
                segments.append((val, "VAL"))

        samples.append(chars_to_bio(segments))
    return samples


# ==================== 硬编码示例（生产模板） ====================

def hardcoded_examples():
    examples = []

    # 单单元格（带/不带标点）
    examples.append(chars_to_bio([
        ("巡检登记", "O"), ("，", "O"),
        ("外径", "SUB"), ("，", "O"),
        ("一号列", "OBJ"), ("，", "O"),
        ("十七点八四", "VAL"),
    ]))
    examples.append(chars_to_bio([
        ("外径", "SUB"), ("二号", "OBJ"), ("十二点零一", "VAL"),
    ]))
    examples.append(chars_to_bio([
        ("内径", "SUB"), ("，", "O"),
        ("三号", "OBJ"), ("填", "O"), ("负零点五", "VAL"),
    ]))

    # 同行多列
    examples.append(chars_to_bio([
        ("外径", "SUB"),
        ("一号", "OBJ"), ("十二点零一", "VAL"),
        ("二号", "OBJ"), ("十二点零八", "VAL"),
        ("三号", "OBJ"), ("十一点九八", "VAL"),
    ]))
    examples.append(chars_to_bio([
        ("表面光洁度", "SUB"), ("，", "O"),
        ("四号", "OBJ"), ("，", "O"), ("一点二", "VAL"),
        ("五号", "OBJ"), ("，", "O"), ("一点一", "VAL"),
    ]))

    # 跨行
    examples.append(chars_to_bio([
        ("巡检登记", "O"), ("，", "O"),
        ("外径", "SUB"), ("一号", "OBJ"), ("十二点零一", "VAL"),
        ("内径", "SUB"), ("一号", "OBJ"), ("十点五五", "VAL"),
    ]))
    examples.append(chars_to_bio([
        ("硬度", "SUB"), ("，", "O"),
        ("六号", "OBJ"), ("，", "O"), ("五十八", "VAL"),
        ("，", "O"),
        ("直线度", "SUB"), ("，", "O"),
        ("二号", "OBJ"), ("，", "O"), ("零点零三", "VAL"),
    ]))

    # 圆度/表面光洁度
    examples.append(chars_to_bio([
        ("圆度", "SUB"), ("三号", "OBJ"), ("零点零一", "VAL"),
    ]))
    examples.append(chars_to_bio([
        ("表面光洁度", "SUB"),
        ("一号", "OBJ"), ("零点八", "VAL"),
        ("二号", "OBJ"), ("零点九", "VAL"),
    ]))

    return examples


# ==================== 主生成函数 ====================

def dedupe(samples, max_len=128):
    seen = set()
    unique = []
    for s in samples:
        text = "".join(ch for ch, _ in s)
        if text not in seen and 3 < len(text.strip()) <= max_len:
            seen.add(text)
            unique.append(s)
    return unique


def generate_all_data(output_dir):
    random.seed(42)

    # ---- 硬性全排列：确保所有 (行标签 × 列号) 常见组合完整覆盖 ----
    print("生成硬性全排列单单元格...")
    hard = inspection_single_full()
    print("生成硬性全排列单主体多列...")
    hard += inspection_single_sub_multi_col()
    print("生成硬性全排列多主体多客体...")
    hard += multi_sub_multi_obj()
    print("生成硬性全排列列前多行...")
    hard += inspection_col_first()
    print("生成硬性全排列序号为主体（序号三/第一个十一点零）...")
    hard += inspection_ordinal_row()
    print("生成【主要类型】行标签+多测量值批量（硬度，测量值一是五十点零...）...")
    hard += inspection_meas_batch()
    hard = dedupe(hard)

    # ---- 随机化组成：覆盖无限变体，扩充数据量 ----
    print("生成随机化组成...")
    rand_parts = [
        inspection_same_row(),
        inspection_with_connectors(),
        inspection_cross_row(),
        inspection_long(),
        generalized_random(2500),
    ]
    rand = []
    for part in rand_parts:
        rand.extend(part)
    rand = dedupe(rand)

    # 保证一半一半：硬性全排列 和 随机化 各采样到相等数量
    print(f"硬性全排列去重后: {len(hard)} 条")
    print(f"随机化去重后: {len(rand)} 条")
    target = min(len(hard), len(rand))
    hard = random.sample(hard, target)
    rand = random.sample(rand, target)

    all_samples = hard + rand
    random.shuffle(all_samples)
    print(f"\n合计 {len(all_samples)} 条（硬性 {len(hard)} + 随机化 {len(rand)}）")

    # 9:1 分割
    split = int(len(all_samples) * 0.9)
    train_data = all_samples[:split]
    val_data = all_samples[split:]

    os.makedirs(output_dir, exist_ok=True)

    def save_bio(data, filepath):
        with open(filepath, "w", encoding="utf-8") as f:
            for sample in data:
                for ch, label in sample:
                    f.write(f"{ch}\t{label}\n")
                f.write("\n")

    save_bio(train_data, os.path.join(output_dir, "train.bio"))
    save_bio(val_data, os.path.join(output_dir, "dev.bio"))

    # 保存 JSON
    def save_json(data, filepath):
        json_data = []
        for sample in data:
            text = "".join(ch for ch, _ in sample)
            entities = []
            ct, ctext = None, ""
            for ch, label in sample:
                if label.startswith("B-"):
                    if ct:
                        entities.append({"type": ct, "text": ctext})
                    ct, ctext = label[2:], ch
                elif label.startswith("I-") and ct == label[2:]:
                    ctext += ch
                else:
                    if ct:
                        entities.append({"type": ct, "text": ctext})
                    ct, ctext = None, ""
            if ct:
                entities.append({"type": ct, "text": ctext})
            json_data.append({"text": text, "entities": entities})
        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(json_data, f, ensure_ascii=False, indent=2)

    save_json(train_data, os.path.join(output_dir, "train.json"))
    save_json(val_data, os.path.join(output_dir, "dev.json"))

    print(f"训练集: {len(train_data)} 条")
    print(f"验证集: {len(val_data)} 条")

    label_counts = {}
    for sample in all_samples:
        for _, label in sample:
            label_counts[label] = label_counts.get(label, 0) + 1
    print(f"\n标签分布:")
    for l in ["O", "B-SUB", "I-SUB", "B-OBJ", "I-OBJ", "B-VAL", "I-VAL"]:
        print(f"  {l}: {label_counts.get(l, 0)}")

    print("\n========== 样例 ==========")
    for i, sample in enumerate(train_data[:15]):
        text = "".join(ch for ch, _ in sample)
        entities = []
        ct, ctext = None, ""
        for ch, label in sample:
            if label.startswith("B-"):
                if ct:
                    entities.append(f"{ct}:{ctext}")
                ct, ctext = label[2:], ch
            elif label.startswith("I-") and ct == label[2:]:
                ctext += ch
            else:
                if ct:
                    entities.append(f"{ct}:{ctext}")
                ct, ctext = None, ""
        if ct:
            entities.append(f"{ct}:{ctext}")
        print(f"\n[{i}] {text}")
        print(f"    -> {' | '.join(entities)}")

    return all_samples


if __name__ == "__main__":
    output_dir = os.path.dirname(os.path.abspath(__file__))
    generate_all_data(output_dir)
    print("\n完成！")