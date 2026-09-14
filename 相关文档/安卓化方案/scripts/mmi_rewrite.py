# 把模型里所有 MatMulInteger(u8,s8) 数学等价重写为 Cast+Sub+MatMul(int32)，
# 绕过 ORT x86_64 上 u8s8 kernel 的 bug，从而验证"量化模型本身"是否正确。
# 用法：python mmi_rewrite.py <in.onnx> <out.onnx>
import sys

import onnx
from onnx import TensorProto as T
from onnx import helper as h

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src, load_external_data=True)

new_nodes = []
count = 0
for n in m.graph.node:
    if n.op_type != 'MatMulInteger':
        new_nodes.append(n)
        continue
    A, B, azp, bzp = n.input[0], n.input[1], n.input[2], n.input[3]
    out = n.output[0]
    p = f'mmi{count}_'
    # A(int/uint) → int32 → 减 azp
    new_nodes.append(h.make_node('Cast', [A], [p + 'a32'], to=T.INT32, name=p + 'castA'))
    new_nodes.append(h.make_node('Cast', [azp], [p + 'azp32'], to=T.INT32, name=p + 'castAZP'))
    new_nodes.append(h.make_node('Sub', [p + 'a32', p + 'azp32'], [p + 'ac'], name=p + 'subA'))
    # B(int8) → int32 → 减 bzp
    new_nodes.append(h.make_node('Cast', [B], [p + 'b32'], to=T.INT32, name=p + 'castB'))
    new_nodes.append(h.make_node('Cast', [bzp], [p + 'bzp32'], to=T.INT32, name=p + 'castBZP'))
    new_nodes.append(h.make_node('Sub', [p + 'b32', p + 'bzp32'], [p + 'bc'], name=p + 'subB'))
    # 等价 MatMul（走正确的 MatMul kernel）
    new_nodes.append(h.make_node('MatMul', [p + 'ac', p + 'bc'], [out], name=p + 'matmul'))
    count += 1

del m.graph.node[:]
m.graph.node.extend(new_nodes)
onnx.checker.check_model(m, full_check=False)
onnx.save(m, dst)
print(f'rewrote {count} MatMulInteger -> Cast/Sub/MatMul, saved {dst}')
