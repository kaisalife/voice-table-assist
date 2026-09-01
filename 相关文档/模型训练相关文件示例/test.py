"""
测试脚本：加载最佳模型，对测试句子进行 NER 预测，提取 主体-客体-数值 三元组
用法：python test.py
"""
import json
import torch
from transformers import BertTokenizerFast
from train import BertCRF, prepare_model

BERT_DIR = prepare_model()
OUTPUT_DIR = "./output"
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# 加载配置
with open(f"{OUTPUT_DIR}/config.json", "r", encoding="utf-8") as f:
    config = json.load(f)

LABEL_LIST = config["label_list"]
ID2LABEL = {int(k): v for k, v in config["id2label"].items()}
MAX_LEN = config["max_len"]

print(f"Labels: {LABEL_LIST}")

# 加载模型
# 注: tokenizer 从 BERT_DIR 加载（output/ 只在训练正常结束时才保存 tokenizer，易被中断缺少）
tokenizer = BertTokenizerFast.from_pretrained(BERT_DIR)
model = BertCRF(BERT_DIR, len(LABEL_LIST))
model.load_state_dict(torch.load(f"{OUTPUT_DIR}/best_model.pt", map_location=DEVICE, weights_only=True))
model.to(DEVICE)
model.eval()
print("模型加载完成\n")


def predict(text):
    """对单句文本进行 NER 预测"""
    tokens = list(text)  # 逐字拆分
    encoding = tokenizer(
        tokens,
        is_split_into_words=True,
        max_length=MAX_LEN,
        padding="max_length",
        truncation=True,
        return_tensors="pt",
    )
    word_ids = encoding.word_ids()

    input_ids = encoding["input_ids"].to(DEVICE)
    attention_mask = encoding["attention_mask"].to(DEVICE)

    with torch.no_grad():
        emissions = model(input_ids, attention_mask)
        decoded = model.decode(emissions, attention_mask)

    pred_tags = decoded[0]  # 取第一个（batch=1）

    # 只保留有效 token 的预测结果
    result = []
    prev_word_idx = None
    for i, word_idx in enumerate(word_ids):
        if word_idx is None or word_idx == prev_word_idx:
            continue
        tag_id = pred_tags[i] if i < len(pred_tags) else 0
        result.append((tokens[word_idx], ID2LABEL.get(tag_id, "O")))
        prev_word_idx = word_idx

    return result


def extract_triples(bio_result):
    """从 BIO 标注结果中提取 主体-客体-数值 三元组"""
    entities = []
    current_type = None
    current_chars = []

    for char, tag in bio_result:
        if tag.startswith("B-"):
            if current_type:
                entities.append(("".join(current_chars), current_type))
            current_type = tag[2:]
            current_chars = [char]
        elif tag.startswith("I-"):
            if current_type == tag[2:]:
                current_chars.append(char)
            else:
                if current_type:
                    entities.append(("".join(current_chars), current_type))
                current_type = None
                current_chars = []
        else:  # O
            if current_type:
                entities.append(("".join(current_chars), current_type))
                current_type = None
                current_chars = []

    if current_type:
        entities.append(("".join(current_chars), current_type))

    # 组装三元组：一个 SUB 后面跟若干 OBJ-VAL 对
    triples = []
    current_sub = None
    i = 0
    while i < len(entities):
        text, etype = entities[i]
        if etype == "SUB":
            current_sub = text
            i += 1
            # 收集这个 SUB 下的所有 OBJ-VAL 对
            while i < len(entities):
                if entities[i][1] == "SUB":
                    break
                if entities[i][1] == "OBJ" and i + 1 < len(entities) and entities[i + 1][1] == "VAL":
                    triples.append((current_sub, entities[i][0], entities[i + 1][0]))
                    i += 2
                elif entities[i][1] == "OBJ":
                    triples.append((current_sub, entities[i][0], "?"))
                    i += 1
                elif entities[i][1] == "VAL":
                    triples.append((current_sub, "?", entities[i][0]))
                    i += 1
                else:
                    i += 1
        else:
            i += 1

    return triples


# ========== 测试用例（生产模板：行标签 + 列号 + 中文数值 0-100 两位小数） ==========
test_sentences = [
    # --- 单单元格（单主体单客体，带/不带标点） ---
    "巡检登记，外径，一号，十七点八四",
    "外径一号十七点八四",
    "内径三号负零点五",
    "巡检登记，表面光洁度，一号列，零点八",
    "硬度二号五十八",

    # --- 单主体多客体（同行多列） ---
    "外径一号十二点零一，二号十二点零八，三号十一点九八",
    "外径一号十二点零一二号十二点零八",
    "表面光洁度一号零点八，二号零点九",
    "硬度一号八十一点零五，三号八点八九",

    # --- 单主体满列（6 客体全覆盖，特训重点） ---
    "外径一号五十六，二号六十七点八零，三号四十八，四号八十九，五号九十点二五，六号七十点二八",
    "外径一号五十六二号六十七点八零三号四十八四号八十九五号九十点二五六号七十点二八",

    # --- 多主体多客体（跨行，每行多列） ---
    "巡检登记，外径一号十二点零一，内径一号十点五五",
    "硬度六号五十八，直线度二号零点零三",
    "外表，硬度二号是八十一点三二，三号设为八点八九，圆度一号六十四点六六",
    "巡检登记，外径四号改成六十八点三四，五号约八十三点四零，内径二号四十六点八零，三号五点二一",

    # --- 列前序（列号在前，行标签在后） ---
    "二号列硬度四十三点八七",
    "巡检登记，五号列圆度九十三点零二",
    "第一列，硬度填十，表面光洁度填十二点零",
    "一号列硬度填十，二号列圆度填零点九",
    # --- 第X列 作为主体（列前 · 单列单行） ---
    "第三列硬度七十八点五六",
    "第六列圆度九十一点零三",
    "第二列外径四十五点零零",
    # --- 第X列 作为主体（多列主体并存 · 各带一或多个行客体） ---
    "第三列硬度填十，第六列圆度填零点九",
    "第二列外径四十五点零零，第五列直线度零点零三",

    # --- 带连接词（是、填、为、等于、约、达到、写、输入、改成、设为） ---
    "外径二号是五十点三",
    "内径一号填四十二点七五",
    "圆度三号为九十点二五",
    "硬度四号等于七十五点四四",
    "直线度一号设为零点零三",
    "表面光洁度一号达到零点九",

    # --- 无标点长句连排（逼近生产难点） ---
    "巡检登记内径四号三十四点六六六号十四点九零",
    "硬度一号八十九点六一圆度三号九十点二五",
    "登记外径一号九十五点七四内径二号十五点三五",

    # --- 测量值X 客体（新增需求：测量值+数字+连接词+数值） ---
    "巡检登记，硬度，测量值一是五十点零，测量值二是四十九点九九，测量值三是五十一点五，测量值四是六十点七五，测量值五是六十六点七五，测量值六，四十八点七七。",
    "硬度，测量值一是五十点零，测量值二是四十九点九九",
    "硬度测量值一五十点零测量值二四十九点九九",
    "巡检登记，硬度，测量值一为五十点零，测量值二等于四十九点九九",
    "巡检登记，圆度，测量值一是九十一点零三，测量值二是八十八点五零",
    "硬度测量值一一百",
    "硬度测量值一负零点五",
    "巡检登记，硬度，测量值一，五十点零，测量值二，四十九点九九",

    # --- 负值 / 边界数值 ---
    "外径一号负零点五",
    "内径二号一百",
    "直线度一号零点零一",
    "硬度三号零点零一",
]

for text in test_sentences:
    bio = predict(text)
    tags_str = " ".join([f"{c}/{t}" for c, t in bio])
    triples = extract_triples(bio)

    print(f"输入: {text}")
    print(f"BIO : {tags_str}")
    if triples:
        for sub, obj, val in triples:
            print(f"  -> 主体: {sub} | 客体: {obj} | 数值: {val}")
    else:
        print(f"  -> 未提取到三元组")
    print()