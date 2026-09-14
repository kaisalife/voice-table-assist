# 大规模精度对比：fp32 基准 vs 量化模型，逐句比较 BIO 是否一致（覆盖 test.txt 全部句子）
import re
import sys

import numpy as np
import onnxruntime as ort

LABELS = ["O", "B-SUB", "I-SUB", "B-OBJ", "I-OBJ", "B-VAL", "I-VAL"]
VOCAB_PATH = 'models/raner/export-int8/vocab.txt'
TEST_TXT = '相关文档/模型训练相关文件示例/test.txt'


def load_vocab():
    v = {}
    for i, l in enumerate(open(VOCAB_PATH, encoding='utf-8')):
        t = l.rstrip('\n')
        if t:
            v[t] = i
    return v


def load_sentences():
    out = []
    for line in open(TEST_TXT, encoding='utf-8'):
        m = re.match(r'\s*输入[:：]\s*(\S.*?)\s*$', line)
        if m:
            out.append(m.group(1))
    return out


def make_feeds(text, vocab):
    ids = [vocab.get('[CLS]', 101)] + [vocab.get(c, vocab.get('[UNK]', 100)) for c in text] + [vocab.get('[SEP]', 102)]
    ids = ids[:128]
    mask = [1] * len(ids) + [0] * (128 - len(ids))
    ids = ids + [0] * (128 - len(ids))
    return {'input_ids': np.array([ids], dtype=np.int64),
            'attention_mask': np.array([mask], dtype=np.int64)}


def bio_of(emissions, text):
    g = emissions.argmax(-1)
    chars = ['[CLS]'] + list(text) + ['[SEP]']
    return [LABELS[g[i]] for i in range(min(len(chars), len(g)))]


def main():
    vocab = load_vocab()
    sentences = load_sentences()
    print(f'测试句数: {len(sentences)}')
    baseline = ort.InferenceSession('models/raner/model.onnx', providers=['CPUExecutionProvider'])
    cand_path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\tang5\AppData\Local\Temp\raner_rr.onnx'
    cand = ort.InferenceSession(cand_path, providers=['CPUExecutionProvider'])

    mismatch = 0
    entity_diff = 0
    for s in sentences:
        f = make_feeds(s, vocab)
        b = bio_of(baseline.run(None, f)[0][0], s)
        c = bio_of(cand.run(None, f)[0][0], s)
        if b != c:
            mismatch += 1
            # 实体层面差异（忽略 O 对齐）
            eb = [(i, t) for i, t in enumerate(b) if t != 'O']
            ec = [(i, t) for i, t in enumerate(c) if t != 'O']
            if eb != ec:
                entity_diff += 1
            if mismatch <= 5:
                print(f'[不一致] {s}')
                print(f'   fp32: {" ".join(b)}')
                print(f'   量化: {" ".join(c)}')
    print(f'\n逐 token BIO 不一致: {mismatch}/{len(sentences)}')
    print(f'实体层面不一致: {entity_diff}/{len(sentences)}')
    print(f'一致率: {100.0*(len(sentences)-mismatch)/len(sentences):.2f}% (token) / '
          f'{100.0*(len(sentences)-entity_diff)/len(sentences):.2f}% (entity)')


if __name__ == '__main__':
    main()
