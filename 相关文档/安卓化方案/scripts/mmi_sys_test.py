# MatMulInteger 内核系统性验证：变化 M/K/N 与数据类型组合，找出 ORT 结果与 numpy 不一致的条件
import numpy as np
import onnx
import onnxruntime as ort
from onnx import helper as h
from onnx import TensorProto as T
from onnx import numpy_helper as nh

print('ORT:', ort.__version__)


def run_case(M, K, N, a_type, b_type, seed=1, azp_val=None, bzp_val=None):
    rng = np.random.default_rng(seed)
    if a_type == T.UINT8:
        A = rng.integers(0, 256, size=(M, K)).astype(np.uint8)
        azp_arr = np.array([128 if azp_val is None else azp_val], dtype=np.uint8)
    else:
        A = rng.integers(-128, 128, size=(M, K)).astype(np.int8)
        azp_arr = np.array([0 if azp_val is None else azp_val], dtype=np.int8)
    if b_type == T.UINT8:
        B = rng.integers(0, 256, size=(K, N)).astype(np.uint8)
        bzp_arr = np.array([128 if bzp_val is None else bzp_val], dtype=np.uint8)
    else:
        B = rng.integers(-128, 128, size=(K, N)).astype(np.int8)
        bzp_arr = np.array([0 if bzp_val is None else bzp_val], dtype=np.int8)

    g = h.make_graph(
        [h.make_node('MatMulInteger', ['a', 'b', 'azp', 'bzp'], ['out'])],
        't', [],
        [h.make_tensor_value_info('out', T.INT32, (M, N))],
        [nh.from_array(A, 'a'), nh.from_array(B, 'b'),
         nh.from_array(azp_arr, 'azp'), nh.from_array(bzp_arr, 'bzp')])
    path = f'C:/Users/tang5/AppData/Local/Temp/mmi_sys.onnx'
    onnx.save(h.make_model(g, opset_imports=[h.make_opsetid('', 13)]), path)
    out = ort.InferenceSession(path, providers=['CPUExecutionProvider']).run(None, {})[0]
    manual = (A.astype(np.int32) - int(azp_arr[0])) @ (B.astype(np.int32) - int(bzp_arr[0]))
    d = int(np.abs(out.astype(np.int64) - manual).max())
    return d


combos = [
    ('u8 x s8', T.UINT8, T.INT8),
    ('s8 x s8', T.INT8, T.INT8),
    ('u8 x u8', T.UINT8, T.UINT8),
]
sizes = [(1, 2, 2), (2, 2, 2), (4, 8, 6), (4, 8, 2), (4, 8, 1), (4, 8, 8),
         (16, 64, 64), (4, 768, 6), (8, 768, 768), (128, 768, 768)]
print(f'{"类型":10s} {"M":>4s} {"K":>4s} {"N":>4s}   maxdiff')
for name, at, bt in combos:
    for (M, K, N) in sizes:
        d = run_case(M, K, N, at, bt)
        flag = '  <-- BAD' if d != 0 else ''
        print(f'{name:10s} {M:4d} {K:4d} {N:4d}   {d}{flag}')
