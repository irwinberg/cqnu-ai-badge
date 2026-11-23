#!/usr/bin/env python3
"""把设计稿素材转成 LVGL 9 RGB565 图片 C 文件（main/assets/images/）。需要 Pillow。"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.environ.get("CQNU_ART_DIR", "/Users/mac/Desktop/重师大/03_产品/AI工牌/设计稿/v3素材/img")
OUT = os.path.join(ROOT, "main", "assets", "images")
SCENES = [("bg_class", "s_class"), ("bg_teach", "s_teach"), ("bg_expo", "s_expo"), ("bg_stage", "s_stage")]
AVATARS = ["p_trainee_a", "p_trainee_b", "p_teacher", "p_player", "p_student", "p_cap1", "p_cap2"]

def fit(img, w, h):
    sw, sh = img.size
    r = max(w / sw, h / sh)
    img = img.resize((round(sw * r), round(sh * r)), Image.LANCZOS)
    l, t = (img.width - w) // 2, (img.height - h) // 2
    return img.crop((l, t, l + w, t + h))

def emit(name, img):
    img = img.convert("RGB")
    w, h = img.size
    data = bytearray()
    for r, g, b in img.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data += bytes((v & 0xFF, v >> 8))
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(f"0x{x:02x}" for x in data[i:i + 24]) + ",")
    src = f"""// 由 tools/cqnu_gen_images.py 生成，勿手改
#include "lvgl.h"
static const uint8_t {name}_map[] = {{
{chr(10).join(lines)}
}};
const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565,
    .header.flags = 0,
    .header.w = {w},
    .header.h = {h},
    .header.stride = {w * 2},
    .data_size = sizeof({name}_map),
    .data = {name}_map,
}};
"""
    with open(os.path.join(OUT, name + ".c"), "w") as f:
        f.write(src)
    return name

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    names = []
    for n, s in SCENES:
        names.append(emit(n, fit(Image.open(os.path.join(SRC, s + ".png")), 240, 320)))
    for i, a in enumerate(AVATARS):
        im = Image.open(os.path.join(SRC, a + ".png"))
        names.append(emit(f"av40_{i}", fit(im, 40, 40)))
        names.append(emit(f"av150_{i}", fit(im, 150, 150)))
    with open(os.path.join(OUT, "cq_images.h"), "w") as f:
        f.write("// 由 tools/cqnu_gen_images.py 生成\n#pragma once\n#include \"lvgl.h\"\n")
        for n in names:
            f.write(f"LV_IMAGE_DECLARE({n});\n")
        f.write(f"#define CQ_AVATAR_COUNT {len(AVATARS)}\n")
    print("images:", len(names))
