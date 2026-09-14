# 最小 RaNER int8 推理复刻（对照 C++ 实现）：vocab.txt 逐字 + [CLS]/[SEP] + numpy Viterbi
# 用法：python raner_check.py [句子] [模型目录]
#   ⚠️ x86 + ORT 上，未 reduce_range 的 dynamic 量化模型会因 u8xs8 kernel 饱和缺陷输出全 O
#      （官方 issue #7524/#16472/#19109）。验收请用：
#        python raner_check.py "硬度一号是十七点八四" models/raner/export-int8-reduced
import json
import sys

import numpy as np
import onnxruntime as ort

INT8_DIR = sys.argv[2] if len(sys.argv) > 2 else "models/raner/export-int8-reduced"
TEXT = sys.argv[1] if len(sys.argv) > 1 else "硬度一号是十七点八四二号测量值六十"

# vocab
vocab = {}
with open(f"{INT8_DIR}/vocab.txt", encoding="utf-8") as f:
    for i, line in enumerate(f):
        t = line.rstrip("\n")
        if t:
            vocab[t] = i

MAX_LEN = 128
ids = [vocab.get("[CLS]", 101)]
for ch in TEXT:
    ids.append(vocab.get(ch, vocab.get("[UNK]", 100)))
ids.append(vocab.get("[SEP]", 102))
mask = [1] * len(ids) + [0] * (MAX_LEN - len(ids))
ids = ids + [0] * (MAX_LEN - len(ids))

sess = ort.InferenceSession(f"{INT8_DIR}/model.onnx", providers=["CPUExecutionProvider"])
print("inputs:", [(i.name, i.type, i.shape) for i in sess.get_inputs()])
print("outputs:", [(o.name, o.shape) for o in sess.get_outputs()])
em = sess.run(None, {"input_ids": np.array([ids], dtype=np.int64),
                     "attention_mask": np.array([mask], dtype=np.int64)})[0][0]
print("emissions shape:", em.shape)

transitions = np.load(f"{INT8_DIR}/crf_transitions.npy")
start_t = np.load(f"{INT8_DIR}/crf_start_transitions.npy")
end_t = np.load(f"{INT8_DIR}/crf_end_transitions.npy")

N = em.shape[-1]
seq_end = int(np.cumprod(mask).sum()) - 1
score = start_t + em[0]
history = []
for t in range(1, seq_end + 1):
    next_score = score[:, None] + transitions + em[t][None, :]
    indices = np.argmax(next_score, axis=0)
    score = next_score[indices, np.arange(N)]
    history.append(indices)
score = score + end_t
best_last = int(np.argmax(score))
path = [best_last]
for t in range(seq_end, 0, -1):
    path.append(int(history[t - 1][path[-1]]))
path = path[::-1]

greedy = [int(np.argmax(em[i])) for i in range(seq_end + 1)]
print("greedy:", "".join(str(g) for g in greedy))
chars = ["<CLS>"] + list(TEXT) + ["<SEP>"]
labels = ["O", "B-SUB", "I-SUB", "B-OBJ", "I-OBJ", "B-VAL", "I-VAL"]
out = [(chars[i], labels[path[i]]) for i in range(len(chars))]
print("viterbi:", " ".join(f"{c}:{l}" for c, l in out if l != "O"))
