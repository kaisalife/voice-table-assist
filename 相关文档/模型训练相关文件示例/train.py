"""
RaNER 微调训练脚本 - ModelScope Notebook 版本
直接运行即可：自动下载模型、处理权重映射、GPU 训练
"""
import os
import json
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from torch.optim import AdamW
from transformers import BertModel, BertTokenizerFast
from torchcrf import CRF
from tqdm import tqdm
import numpy as np

# ========== 配置 ==========

# ModelScope 模型 ID（自动下载，无需手动上传模型文件）
MODEL_ID = "damo/nlp_raner_named-entity-recognition_chinese-base-news"
MODEL_CACHE_DIR = "./model_cache"      # 下载后的模型存放位置
BERT_DIR = "./bert_remapped"           # 权重映射后的 BERT 路径
OUTPUT_DIR = "./output"
DATA_DIR = "."

# 7 类标签
LABEL_LIST = ["O", "B-SUB", "I-SUB", "B-OBJ", "I-OBJ", "B-VAL", "I-VAL"]
LABEL2ID = {l: i for i, l in enumerate(LABEL_LIST)}
ID2LABEL = {i: l for i, l in enumerate(LABEL_LIST)}
NUM_LABELS = len(LABEL_LIST)

# 训练参数
MAX_LEN = 128
BATCH_SIZE = 128                      # GPU 训练，加大 batch
NUM_WORKERS = 16                       # 数据加载并行
EPOCHS = 10
LR = 2e-5
WARMUP_RATIO = 0.1
GRADIENT_ACCUMULATION_STEPS = 1
PATIENCE = 5
# 「与上次训练结果比较」的积极早停：
# 若本次累积最优 F1 未超过上次，且与上次差距在容差内，则提前结束训练
MIN_EPOCHS = 3                         # 至少训练的 epoch 数，避免首轮误判
STOP_TOLERANCE = 0.01                  # 与上次 F1 的差距容差（不足即视为差距不大）
PREV_RESULT_FILE = os.path.join(OUTPUT_DIR, "prev_best_f1.json")   # 记录上一次训练的最优 F1
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# —— GPU 加速 ——
if DEVICE.type == "cuda":
    torch.backends.cudnn.benchmark = True
    torch.set_float32_matmul_precision("high")
USE_AMP = DEVICE.type == "cuda"        # 混合精度仅在 CUDA 下启用
scaler = torch.amp.GradScaler("cuda") if USE_AMP else None

print(f"Device: {DEVICE}")
print(f"Labels: {LABEL_LIST}")


# ========== 步骤 1：下载并准备模型 ==========

def prepare_model():
    """下载 RaNER 模型并修复权重 key 映射，使得 BertModel.from_pretrained 可以直接加载"""
    if os.path.exists(BERT_DIR) and os.path.exists(os.path.join(BERT_DIR, "pytorch_model.bin")):
        print(f"[模型] 已存在 remapped 权重: {BERT_DIR}")
        return BERT_DIR

    print(f"[模型] 从 ModelScope 下载: {MODEL_ID}")
    from modelscope import snapshot_download
    model_dir = snapshot_download(MODEL_ID, cache_dir=MODEL_CACHE_DIR)
    print(f"[模型] 下载完成: {model_dir}")

    # 加载原始权重
    ckpt_path = os.path.join(model_dir, "pytorch_model.bin")
    ckpt = torch.load(ckpt_path, map_location="cpu")

    # 重映射 key: encoder.xxx -> bert.xxx
    new_ckpt = {}
    for k, v in ckpt.items():
        # 跳过 CRF 和 linear 层（我们会创建新的）
        if k.startswith("crf.") or k.startswith("linear."):
            continue
        # encoder.embeddings.xxx    -> bert.embeddings.xxx
        # encoder.encoder.layer.N.xxx -> bert.encoder.layer.N.xxx
        # encoder.pooler.xxx        -> bert.pooler.xxx
        new_k = k.replace("encoder.embeddings", "bert.embeddings")
        new_k = new_k.replace("encoder.encoder.layer", "bert.encoder.layer")
        new_k = new_k.replace("encoder.pooler", "bert.pooler")
        new_ckpt[new_k] = v

    os.makedirs(BERT_DIR, exist_ok=True)
    torch.save(new_ckpt, os.path.join(BERT_DIR, "pytorch_model.bin"))

    # 复制 config 和 vocab 文件
    import shutil
    for fname in ["config.json", "vocab.txt", "tokenizer_config.json"]:
        src = os.path.join(model_dir, fname)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(BERT_DIR, fname))

    print(f"[模型] 权重映射完成，保存到: {BERT_DIR} ({len(new_ckpt)} keys)")
    return BERT_DIR


# ========== 模型定义 ==========

class BertCRF(nn.Module):
    def __init__(self, bert_dir, num_labels, dropout=0.1):
        super().__init__()
        self.num_labels = num_labels
        self.bert = BertModel.from_pretrained(bert_dir)
        self.dropout = nn.Dropout(dropout)
        self.classifier = nn.Linear(self.bert.config.hidden_size, num_labels)
        self.crf = CRF(num_labels, batch_first=True)

        nn.init.xavier_uniform_(self.classifier.weight)
        nn.init.zeros_(self.classifier.bias)

    def forward(self, input_ids, attention_mask, labels=None):
        outputs = self.bert(input_ids=input_ids, attention_mask=attention_mask)
        sequence_output = self.dropout(outputs.last_hidden_state)
        emissions = self.classifier(sequence_output)

        if labels is not None:
            mask = attention_mask.bool()
            crf_labels = labels.clone()
            crf_labels[crf_labels == -100] = 0
            loss = -self.crf(emissions, crf_labels, mask=mask, reduction='mean')
            return loss, emissions
        else:
            return emissions

    def decode(self, emissions, attention_mask):
        mask = attention_mask.bool()
        return self.crf.decode(emissions, mask=mask)


# ========== 数据集 ==========

class NERDataset(Dataset):
    def __init__(self, filepath, tokenizer, max_len=128):
        self.tokenizer = tokenizer
        self.max_len = max_len
        self.samples = self._load_bio(filepath)

    def _load_bio(self, filepath):
        samples = []
        with open(filepath, "r", encoding="utf-8") as f:
            tokens, labels = [], []
            for line in f:
                line = line.strip()
                if not line:
                    if tokens:
                        samples.append((tokens, labels))
                        tokens, labels = [], []
                else:
                    parts = line.split("\t")
                    if len(parts) == 2:
                        tokens.append(parts[0])
                        labels.append(parts[1])
            if tokens:
                samples.append((tokens, labels))
        return samples

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx):
        tokens, labels = self.samples[idx]

        encoding = self.tokenizer(
            tokens,
            is_split_into_words=True,
            max_length=self.max_len,
            padding="max_length",
            truncation=True,
            return_tensors="pt",
        )

        input_ids = encoding["input_ids"].squeeze(0)
        attention_mask = encoding["attention_mask"].squeeze(0)
        word_ids = encoding.word_ids()

        aligned_labels = []
        prev_word_idx = None
        for word_idx in word_ids:
            if word_idx is None:
                aligned_labels.append(-100)
            elif word_idx != prev_word_idx:
                aligned_labels.append(LABEL2ID.get(labels[word_idx], 0))
            else:
                orig_label = labels[word_idx]
                if orig_label.startswith("B-"):
                    aligned_labels.append(LABEL2ID.get("I-" + orig_label[2:], 0))
                else:
                    aligned_labels.append(LABEL2ID.get(orig_label, 0))
            prev_word_idx = word_idx

        if len(aligned_labels) > self.max_len:
            aligned_labels = aligned_labels[:self.max_len]
        else:
            aligned_labels += [-100] * (self.max_len - len(aligned_labels))

        return {
            "input_ids": input_ids,
            "attention_mask": attention_mask,
            "labels": torch.tensor(aligned_labels, dtype=torch.long),
        }


# ========== 评估 ==========

def compute_metrics(true_labels, pred_labels, label_list):
    true_entities = _extract_entities(true_labels, label_list)
    pred_entities = _extract_entities(pred_labels, label_list)

    correct = 0
    for ent in pred_entities:
        if ent in true_entities:
            correct += 1
            true_entities.remove(ent)

    total_pred = len(pred_entities)
    total_true = correct + len(true_entities)

    precision = correct / total_pred if total_pred > 0 else 0
    recall = correct / total_true if total_true > 0 else 0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

    return precision, recall, f1


def _extract_entities(labels, label_list):
    entities = []
    current_type = None
    start = -1
    for i, label_id in enumerate(labels):
        if label_id == -100:
            continue
        label = label_list[label_id]
        if label.startswith("B-"):
            if current_type:
                entities.append((current_type, start, i))
            current_type = label[2:]
            start = i
        elif label.startswith("I-"):
            if current_type != label[2:]:
                if current_type:
                    entities.append((current_type, start, i))
                current_type = None
        else:
            if current_type:
                entities.append((current_type, start, i))
                current_type = None
    if current_type:
        entities.append((current_type, start, len(labels)))
    return entities


def evaluate(model, dataloader):
    model.eval()
    all_true, all_pred = [], []
    total_loss = 0

    with torch.no_grad():
        for batch in dataloader:
            input_ids = batch["input_ids"].to(DEVICE)
            attention_mask = batch["attention_mask"].to(DEVICE)
            labels = batch["labels"].to(DEVICE)

            loss, emissions = model(input_ids, attention_mask, labels)
            total_loss += loss.item()

            decoded = model.decode(emissions, attention_mask)

            for i in range(len(decoded)):
                true_seq = labels[i].cpu().tolist()
                pred_seq = [-100] * len(true_seq)
                for j, tag in enumerate(decoded[i]):
                    pred_seq[j] = tag
                all_true.append(true_seq)
                all_pred.append(pred_seq)

    flat_true = [t for seq in all_true for t in seq]
    flat_pred = [p for seq in all_pred for p in seq]

    precision, recall, f1 = compute_metrics(flat_true, flat_pred, LABEL_LIST)
    avg_loss = total_loss / len(dataloader)

    return avg_loss, precision, recall, f1


def load_prev_best_f1():
    """读取上一次训练的最优 F1；无记录时返回 None"""
    try:
        with open(PREV_RESULT_FILE, "r", encoding="utf-8") as f:
            return float(json.load(f)["best_f1"])
    except Exception:
        return None


def save_prev_best_f1(f1):
    """记录本次训练的最优 F1，供下次训练对比"""
    with open(PREV_RESULT_FILE, "w", encoding="utf-8") as f:
        json.dump({"best_f1": round(float(f1), 4)}, f)


# ========== 训练 ==========

def train():
    # 1. 准备模型
    bert_dir = prepare_model()

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    tokenizer = BertTokenizerFast.from_pretrained(bert_dir)

    # 2. 加载数据
    train_dataset = NERDataset(os.path.join(DATA_DIR, "train.bio"), tokenizer, MAX_LEN)
    val_dataset = NERDataset(os.path.join(DATA_DIR, "dev.bio"), tokenizer, MAX_LEN)

    train_loader = DataLoader(train_dataset, batch_size=BATCH_SIZE, shuffle=True,
                              num_workers=NUM_WORKERS, pin_memory=True, prefetch_factor=2)
    val_loader = DataLoader(val_dataset, batch_size=BATCH_SIZE, shuffle=False,
                            num_workers=NUM_WORKERS, pin_memory=True, prefetch_factor=2)

    print(f"训练集: {len(train_dataset)} 条, 验证集: {len(val_dataset)} 条")

    # 3. 创建模型
    model = BertCRF(bert_dir, NUM_LABELS, dropout=0.1)
    model.to(DEVICE)

    # 4. 优化器
    no_decay = ["bias", "LayerNorm.weight"]
    optimizer_grouped_parameters = [
        {
            "params": [p for n, p in model.named_parameters() if not any(nd in n for nd in no_decay)],
            "weight_decay": 0.01,
        },
        {
            "params": [p for n, p in model.named_parameters() if any(nd in n for nd in no_decay)],
            "weight_decay": 0.0,
        },
    ]
    optimizer = AdamW(optimizer_grouped_parameters, lr=LR)

    total_steps = len(train_loader) * EPOCHS // GRADIENT_ACCUMULATION_STEPS
    warmup_steps = int(total_steps * WARMUP_RATIO)
    scheduler = torch.optim.lr_scheduler.OneCycleLR(
        optimizer, max_lr=LR, total_steps=total_steps,
        pct_start=WARMUP_RATIO, anneal_strategy='cos'
    )

    best_f1 = 0
    best_epoch = 0
    patience_counter = 0
    prev_best_f1 = load_prev_best_f1()   # 上次训练最优 F1（无记录=None），作为本次对比基准
    if prev_best_f1 is not None:
        print(f"[对比] 上次训练最优 F1: {prev_best_f1:.4f}，本次未超越且差距 <={STOP_TOLERANCE:.3f} 时将提前结束")

    for epoch in range(1, EPOCHS + 1):
        model.train()
        total_loss = 0
        optimizer.zero_grad()

        pbar = tqdm(train_loader, desc=f"Epoch {epoch}/{EPOCHS}")
        for step, batch in enumerate(pbar):
            input_ids = batch["input_ids"].to(DEVICE)
            attention_mask = batch["attention_mask"].to(DEVICE)
            labels = batch["labels"].to(DEVICE)

            with torch.amp.autocast("cuda", enabled=USE_AMP):
                loss, _ = model(input_ids, attention_mask, labels)
            loss = loss / GRADIENT_ACCUMULATION_STEPS

            if USE_AMP:
                scaler.scale(loss).backward()
            else:
                loss.backward()

            total_loss += loss.item()

            if (step + 1) % GRADIENT_ACCUMULATION_STEPS == 0:
                if USE_AMP:
                    scaler.unscale_(optimizer)
                    torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
                    scaler.step(optimizer)
                    scaler.update()
                else:
                    torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
                    optimizer.step()
                scheduler.step()
                optimizer.zero_grad()

            pbar.set_postfix({"loss": f"{loss.item():.4f}", "lr": f"{scheduler.get_last_lr()[0]:.2e}"})

        val_loss, val_precision, val_recall, val_f1 = evaluate(model, val_loader)
        print(f"Epoch {epoch} - Val Loss: {val_loss:.4f}, P: {val_precision:.4f}, R: {val_recall:.4f}, F1: {val_f1:.4f}")

        if val_f1 > best_f1:
            best_f1 = val_f1
            best_epoch = epoch
            patience_counter = 0
            torch.save(model.state_dict(), os.path.join(OUTPUT_DIR, "best_model.pt"))
            save_prev_best_f1(val_f1)   # 更新持久化记录，供下次训练对比（中断也能保留最优）
            print(f"  -> 保存最佳模型 (F1={best_f1:.4f})")
        else:
            patience_counter += 1
            # 积极早停：本次累积最优未超过上次，且两者差距不大 → 直接结束
            same_as_prev = (
                prev_best_f1 is not None
                and epoch >= MIN_EPOCHS
                and best_f1 <= prev_best_f1
                and (prev_best_f1 - best_f1) <= STOP_TOLERANCE
            )
            if same_as_prev:
                print(f"较上次({prev_best_f1:.4f})未提升且差距不大(当前最优 {best_f1:.4f})，结束训练于 epoch {epoch}")
                break
            if patience_counter >= PATIENCE:
                print(f"早停于 epoch {epoch}")
                break

    print(f"\n训练完成！最佳 F1: {best_f1:.4f} (epoch {best_epoch})")

    config = {
        "label_list": LABEL_LIST,
        "label2id": LABEL2ID,
        "id2label": ID2LABEL,
        "max_len": MAX_LEN,
        "best_f1": best_f1,
    }
    with open(os.path.join(OUTPUT_DIR, "config.json"), "w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=2)

    tokenizer.save_pretrained(OUTPUT_DIR)

    return model, best_f1


if __name__ == "__main__":
    train()