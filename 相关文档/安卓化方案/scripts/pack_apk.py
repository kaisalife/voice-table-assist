# 组装最终 APK：把 aapt2 base.apk 的条目 + classes.dex + lib/*.so 写成一个新 zip。
# 用 Python 全程可控（.NET ZipFile 更新会破坏 aapt2 的资源索引，导致 AssetManager 打不开）。
# 用法：python pack_apk.py <base.apk> <classes.dex> <libDir> <abi> <out.apk>
import os
import sys
import zipfile

base, dex, libdir, abi, out = sys.argv[1:6]

with zipfile.ZipFile(base, 'r') as zin, zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        # 保留原压缩方式（resources.arsc 必须保持 aapt2 的存储方式）
        if item.compress_type == zipfile.ZIP_STORED:
            zout.writestr(item, data, compress_type=zipfile.ZIP_STORED)
        else:
            zout.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED)
    # classes.dex（存储，便于 ART 直接映射）
    with open(dex, 'rb') as f:
        zout.writestr('classes.dex', f.read(), compress_type=zipfile.ZIP_STORED)
    # 原生库（存储，zipalign 会对齐）
    for name in sorted(os.listdir(libdir)):
        p = os.path.join(libdir, name)
        if os.path.isfile(p):
            with open(p, 'rb') as f:
                zout.writestr('lib/%s/%s' % (abi, name), f.read(), compress_type=zipfile.ZIP_STORED)

print('packed ->', out, os.path.getsize(out) // (1024 * 1024), 'MB')
