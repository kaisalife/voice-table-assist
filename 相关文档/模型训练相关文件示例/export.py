"""
导出模型为 ONNX 格式，供 C# 后端调用。
导出内容：
  1. model.onnx        — BERT + Linear 部分（输出 emissions）
  2. crf_transitions.npy — CRF 转移矩阵（C# 端做 Viterbi 解码）
  3. config.json        — 标签列表、max_len 等配置
  4. vocab.txt, tokenizer.json — 分词器文件
"""
import json
import os
import shutil
import torch
import numpy as np
from transformers import BertTokenizerFast
from train import BertCRF, LABEL_LIST, NUM_LABELS, MAX_LEN, prepare_model

BERT_DIR = prepare_model()
OUTPUT_DIR = "./output"
EXPORT_DIR = "./export"
os.makedirs(EXPORT_DIR, exist_ok=True)

DEVICE = torch.device("cpu")

# 1. 加载模型
model = BertCRF(BERT_DIR, NUM_LABELS)
model.load_state_dict(torch.load(f"{OUTPUT_DIR}/best_model.pt", map_location=DEVICE, weights_only=True))
model.to(DEVICE)
model.eval()
print("模型加载完成")

# 2. 导出 CRF 转移矩阵
crf_transitions = model.crf.transitions.detach().cpu().numpy()
np.save(f"{EXPORT_DIR}/crf_transitions.npy", crf_transitions)
print(f"CRF 转移矩阵: {crf_transitions.shape} → {EXPORT_DIR}/crf_transitions.npy")

# 3. 定义一个只输出 emissions 的模型（去掉 CRF）
class BertEmissions(torch.nn.Module):
    def __init__(self, bert_crf):
        super().__init__()
        self.bert = bert_crf.bert
        self.classifier = bert_crf.classifier

    def forward(self, input_ids, attention_mask):
        outputs = self.bert(input_ids=input_ids, attention_mask=attention_mask)
        last_hidden = outputs.last_hidden_state
        emissions = self.classifier(last_hidden)
        return emissions

emissions_model = BertEmissions(model)
emissions_model.eval()

# 4. 导出 ONNX
dummy_input_ids = torch.randint(0, 2000, (1, MAX_LEN), dtype=torch.long)
dummy_attention_mask = torch.ones(1, MAX_LEN, dtype=torch.long)

onnx_path = f"{EXPORT_DIR}/model.onnx"
torch.onnx.export(
    emissions_model,
    (dummy_input_ids, dummy_attention_mask),
    onnx_path,
    input_names=["input_ids", "attention_mask"],
    output_names=["emissions"],
    dynamic_axes={
        "input_ids": {0: "batch", 1: "seq_len"},
        "attention_mask": {0: "batch", 1: "seq_len"},
        "emissions": {0: "batch", 1: "seq_len"},
    },
    opset_version=14,
    do_constant_folding=True,
)
print(f"ONNX 模型: {EXPORT_DIR}/model.onnx")

# 5. 复制 tokenizer 和配置文件
for fname in ["vocab.txt", "tokenizer.json", "tokenizer_config.json", "special_tokens_map.json"]:
    src = os.path.join(BERT_DIR, fname) if os.path.exists(os.path.join(BERT_DIR, fname)) else os.path.join(OUTPUT_DIR, fname)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(EXPORT_DIR, fname))
        print(f"复制: {fname}")

# 6. 保存标签配置
config = {
    "label_list": LABEL_LIST,
    "num_labels": NUM_LABELS,
    "max_len": MAX_LEN,
    "label2id": {l: i for i, l in enumerate(LABEL_LIST)},
    "id2label": {i: l for i, l in enumerate(LABEL_LIST)},
}
with open(f"{EXPORT_DIR}/config.json", "w", encoding="utf-8") as f:
    json.dump(config, f, ensure_ascii=False, indent=2)
print(f"配置: {EXPORT_DIR}/config.json")

# 7. 验证 ONNX 模型
import onnx
import onnxruntime as ort

onnx_model = onnx.load(onnx_path)
onnx.checker.check_model(onnx_model)
print("ONNX 模型验证通过")

# 与 PyTorch 输出对比验证
ort_session = ort.InferenceSession(onnx_path)
ort_inputs = {
    "input_ids": dummy_input_ids.numpy(),
    "attention_mask": dummy_attention_mask.numpy(),
}
ort_outputs = ort_session.run(None, ort_inputs)

with torch.no_grad():
    pt_outputs = emissions_model(dummy_input_ids, dummy_attention_mask)

diff = np.abs(ort_outputs[0] - pt_outputs.numpy()).max()
print(f"PyTorch vs ONNX 最大误差: {diff:.6f}")

print(f"\n导出完成！文件位于 {EXPORT_DIR}/")
print(f"  model.onnx    — 模型（BERT + Linear）")
print(f"  crf_transitions.npy — CRF 转移矩阵")
print(f"  config.json   — 标签配置")
print(f"  vocab.txt 等  — 分词器")