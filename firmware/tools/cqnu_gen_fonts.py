#!/usr/bin/env python3
"""生成 AI 工牌用的 LVGL 字体（v2，适老化：正文 16px 起）。输出到 main/assets/fonts/。

字体源文件不入库，默认从 CQNU_FONT_DIR 读取：阿里巴巴普惠体 3.0（粗 / 特粗）、演示流云楷、Fusion Pixel 12px。
收字范围：国标 6763 字 + 常用符号 + 界面源码里出现的字 + 后台多维表格里出现的字（人名里的生僻字等）。
后台有新名单时重新运行本脚本再编译一次，名字里的生僻字就不会显示成方框。"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "main", "assets", "fonts")
FONT_DIR = os.environ.get("CQNU_FONT_DIR", "/Users/mac/Desktop/重师大/03_产品/AI工牌/设计稿/fonts")
CONV = os.environ.get("LV_FONT_CONV", os.path.expanduser("~/esp/node-tools/node_modules/.bin/lv_font_conv"))
PH_B = os.path.join(FONT_DIR, "AlibabaPuHuiTi-3-85-Bold.ttf")
PH_H = os.path.join(FONT_DIR, "AlibabaPuHuiTi-3-105-Heavy.ttf")
HW = os.path.join(FONT_DIR, "演示流云楷.ttf")
PX = os.path.join(FONT_DIR, "fusion-pixel-12px-proportional-zh_hans.otf")

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
PUNCT = "·、，。！？：；“”‘’（）《》【】—…～￥％＋－×÷℃・〜「」『』°"


def gb2312(level2=True, symbols=True):
    chars = []
    rows = (list(range(0xA1, 0xAA)) if symbols else []) + list(range(0xB0, 0xF8 if level2 else 0xD8))
    for hi in rows:
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(chars)


def source_chars():
    """界面源码字符串里的非 ASCII 字。"""
    chars = set()
    for f in ("app_ui.c", "app_model.c"):
        s = open(os.path.join(ROOT, "main", f), encoding="utf-8").read()
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', s):
            chars.update(ch for ch in lit if ord(ch) > 127)
    return "".join(sorted(chars))


def base_chars():
    """后台多维表格里出现的所有字（需要 server/config.json 和已登录的 lark-cli）。读不到就跳过。"""
    try:
        sys.path.insert(0, os.path.join(ROOT, "server"))
        sys.path.insert(0, os.path.join(os.path.dirname(ROOT), "server"))   # 合并仓库里 server/ 和 firmware/ 同级
        import badge_server as bs
        bs.refresh_once()
        chars = set()
        for rows in bs._cache["tables"].values():
            for r in rows:
                for k, v in r.items():
                    if k == "_id":
                        continue
                    chars.update(ch for ch in bs.txt(v) if ord(ch) > 127)
        return "".join(sorted(chars))
    except Exception as e:
        print("（跳过后台文字：%s）" % e)
        return ""


def available(font_path, text):
    """只保留字体里真有的字，避免方框占位。"""
    from fontTools.ttLib import TTFont
    cmap = TTFont(font_path, lazy=True).getBestCmap() or {}
    keep = "".join(sorted({ch for ch in text if ord(ch) in cmap}))
    missing = sorted({ch for ch in text if ord(ch) not in cmap and ch.strip()})
    return keep, missing


def run(name, font, size, text, bpp=4):
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, name + ".c")
    sym, missing = available(font, text)
    cmd = [CONV, "--no-prefilter", "--bpp", str(bpp), "--size", str(size), "--format", "lvgl", "--lv-font-name", name,
           "--font", font, "--symbols", sym, "-o", out, "--lv-include", "lvgl.h"]
    subprocess.run(cmd, check=True, capture_output=True)
    extra = ("，字体里缺 %d 个：%s" % (len(missing), "".join(missing[:20]))) if missing else ""
    print(f"{name:10s} {len(sym):5d} 字  {os.path.getsize(out) / 1024:7.0f} KB 源码{extra}", flush=True)


if __name__ == "__main__":
    for old in ("cq_f12", "cq_f14", "cq_f17", "cq_px12"):
        p = os.path.join(OUT, old + ".c")
        if os.path.exists(p):
            os.remove(p)
    src = source_chars()
    base = base_chars()
    full = ASCII + PUNCT + gb2312(True) + src + base
    run("cq_f16", PH_B, 16, full)
    run("cq_f20", PH_H, 20, full)
    run("cq_f36", PH_H, 36, ASCII + "·" + src)
    run("cq_hw40", HW, 40, ASCII + "·" + gb2312(False, False) + base, bpp=2)
    run("cq_px24", PX, 24, ASCII, bpp=1)
    run("cq_px48", PX, 48, ASCII, bpp=1)
    print("FONTS_DONE", flush=True)
