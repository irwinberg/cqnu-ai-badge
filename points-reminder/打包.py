#!/usr/bin/env python3
"""打学员安装包：dist/points-reminder.zip（文件名用 UTF-8 存，Windows 解压不乱码）。

只打这几个文件；登录信息在 ~/.points-reminder 里，本来就不在这个文件夹，不会被打进去。
用法：python3 打包.py
"""
import os, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["SKILL.md", "模板/index.html", "自动领取.py", "工牌提醒.py"]
out_dir = os.path.join(HERE, "dist")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "points-reminder.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for f in FILES:
        z.write(os.path.join(HERE, f), "points-reminder/" + f)
with zipfile.ZipFile(out) as z:
    bad = [i.filename for i in z.infolist() if any(ord(c) > 127 for c in i.filename) and not i.flag_bits & 0x800]
    assert not bad, "这些文件名没按 UTF-8 存：%s" % bad
    print("已打包：%s（%d 个文件，%.0f KB）" % (out, len(z.namelist()), os.path.getsize(out) / 1024))
    for n in z.namelist():
        print("  ", n)
