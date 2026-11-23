#!/usr/bin/env python3
"""用 Chrome 无头模式把矢量图标渲染成透明 PNG，再转成 LVGL A8 图片（可在代码里改颜色）。"""
import os, subprocess, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "main", "assets", "images")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
I = {
 "home": '<path d="M8 2 1.5 7.5h2V14h3.5v-4h2v4H12.5V7.5h2z"/>',
 "book": '<path d="M1.5 3c2.5-.6 4.5-.2 6 1v9c-1.5-1-3.5-1.4-6-.9zM14.5 3c-2.5-.6-4.5-.2-6 1v9c1.5-1 3.5-1.4 6-.9z"/>',
 "clip": '<path d="M5 2.5h6v2H5z"/><path d="M3 3.5h1.5V5.5h7V3.5H13V15H3zM5 8v1.2h6V8zm0 2.8V12h4v-1.2z" fill-rule="evenodd"/>',
 "user": '<circle cx="8" cy="5" r="3"/><path d="M2.5 15c0-3.3 2.4-5.5 5.5-5.5s5.5 2.2 5.5 5.5z"/>',
 "chart": '<path d="M2 9h3v5H2zM6.5 5h3v9h-3zM11 2h3v12h-3z"/>',
 "bell": '<path d="M8 1.5c.6 0 1 .4 1 1v.6c2.1.5 3.5 2.3 3.5 4.6V11l1.5 1.5v.8H2v-.8L3.5 11V7.7c0-2.3 1.4-4.1 3.5-4.6v-.6c0-.6.4-1 1-1zM6.3 14.2h3.4a1.7 1.7 0 0 1-3.4 0z"/>',
 "pic": '<path fill-rule="evenodd" d="M1.5 2.5h13v11h-13zm1.5 1.5v6.8l3-3 2.5 2.5 2-2 2.5 2.5V4z"/><circle cx="10.5" cy="6" r="1.3"/>',
 "heart": '<path d="M8 14 2.2 8.3A3.4 3.4 0 0 1 8 3.6a3.4 3.4 0 0 1 5.8 4.7z"/>',
 "trophy": '<path fill-rule="evenodd" d="M4 1.5h8v1.5h2.5v1.5c0 2-1.4 3.5-3.2 3.8A4 4 0 0 1 9 10.4V12h2.5v2.5h-7V12H7v-1.6a4 4 0 0 1-2.3-2.1C2.9 8 1.5 6.5 1.5 4.5V3H4zM2.9 4.4c.1 1 .6 1.8 1.3 2.2L4 4.4zM13.1 4.4H12l-.2 2.2c.7-.4 1.2-1.2 1.3-2.2z"/>',
 "list": '<path d="M1.5 3h2v2h-2zM5 3.4h9.5v1.2H5zM1.5 7h2v2h-2zM5 7.4h9.5v1.2H5zM1.5 11h2v2h-2zM5 11.4h9.5v1.2H5z"/>',
 "coin": '<path fill-rule="evenodd" d="M8 1.5a6.5 6.5 0 1 1 0 13 6.5 6.5 0 0 1 0-13zm-.7 3.5v6h1.4V5z"/>',
 "qr": '<path d="M2 2h5v5H2zm1.3 1.3v2.4h2.4V3.3zM9 2h5v5H9zm1.3 1.3v2.4h2.4V3.3zM2 9h5v5H2zm1.3 1.3v2.4h2.4v-2.4zM9 9h2v2H9zm3 0h2v2h-2zm-3 3h2v2H9zm3 0h2v2h-2z" fill-rule="evenodd"/>',
 "star": '<path d="M8 1.2l2 4.3 4.7.5-3.5 3.2 1 4.6L8 11.5l-4.2 2.3 1-4.6L1.3 6l4.7-.5z"/>',
 "check": '<path d="M6.3 12.2 2.4 8.3l1.4-1.4 2.5 2.5 6-6 1.4 1.4z"/>',
 "clock": '<path fill-rule="evenodd" d="M8 1.5a6.5 6.5 0 1 1 0 13 6.5 6.5 0 0 1 0-13zm-.7 3v4.1l3.2 2 .7-1.1-2.5-1.6V4.5z"/>',
 "flag": '<path d="M3 1.5h1.5v13H3zM5 2h8l-2 3 2 3H5z"/>',
 "gear": '<path fill-rule="evenodd" d="M7 1h2l.4 1.9 1.3.6 1.7-1 1.4 1.4-1 1.7.6 1.3L15 7v2l-1.9.4-.6 1.3 1 1.7-1.4 1.4-1.7-1-1.3.6L9 15H7l-.4-1.9-1.3-.6-1.7 1-1.4-1.4 1-1.7-.6-1.3L1 9V7l1.9-.4.6-1.3-1-1.7 1.4-1.4 1.7 1 1.3-.6zm1 4.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5z"/>',
 "back": '<path d="M7 3 2 8l5 5V9.6h7V6.4H7z"/>',
 "upload": '<path d="M8 1.5 3.5 6h3v5h3V6h3zM2.5 12.5h11V15h-11z"/>',
 "crown": '<path d="M1 13 0 4l4.5 3.2L8 1.5l3.5 5.7L16 4l-1 9z"/>',
 "wifi": '<path d="M8 13.5 6 11.3a2.8 2.8 0 0 1 4 0zM3.6 8.8a6.2 6.2 0 0 1 8.8 0l-1.3 1.3a4.4 4.4 0 0 0-6.2 0zM1 6.2a9.9 9.9 0 0 1 14 0l-1.3 1.3a8 8 0 0 0-11.4 0z"/>',
 "phone": '<path fill-rule="evenodd" d="M4.5 1h7v14h-7zm1.3 1.5v10h4.4v-10zM7.3 13.2h1.4v.9H7.3z"/>',
}
SIZES = {"s": 16, "m": 26, "l": 40}

def render(size):
    cell = size + 8
    names = list(I)
    html = "<html><body style='margin:0;background:transparent'>" + "".join(
        f"<svg style='position:absolute;left:{4 + i * cell}px;top:4px' width='{size}' height='{size}' viewBox='0 0 16 16' fill='#fff'>{I[n]}</svg>"
        for i, n in enumerate(names)) + "</body></html>"
    d = tempfile.mkdtemp()
    hp, pp = os.path.join(d, "i.html"), os.path.join(d, "i.png")
    open(hp, "w").write(html)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--default-background-color=00000000", f"--window-size={cell * len(names) + 8},{size + 8}",
                    f"--screenshot={pp}", "file://" + hp], capture_output=True, timeout=60)
    im = Image.open(pp).convert("RGBA")
    return {n: im.crop((4 + i * cell, 4, 4 + i * cell + size, 4 + size)) for i, n in enumerate(names)}

def emit_a8(name, im):
    w, h = im.size
    a = bytes(px[3] for px in im.getdata())
    rows = ["    " + ",".join(f"0x{x:02x}" for x in a[i:i + 24]) + "," for i in range(0, len(a), 24)]
    return f"""static const uint8_t {name}_map[] = {{
{chr(10).join(rows)}
}};
const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC, .header.cf = LV_COLOR_FORMAT_A8, .header.flags = 0,
    .header.w = {w}, .header.h = {h}, .header.stride = {w},
    .data_size = sizeof({name}_map), .data = {name}_map,
}};
"""

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    src = ["// 由 tools/cqnu_gen_icons.py 生成，勿手改\n#include \"lvgl.h\"\n"]
    hdr = ["// 由 tools/cqnu_gen_icons.py 生成\n#pragma once\n#include \"lvgl.h\"\n"]
    for key, size in SIZES.items():
        for n, im in render(size).items():
            nm = f"ic_{key}_{n}"
            src.append(emit_a8(nm, im)); hdr.append(f"LV_IMAGE_DECLARE({nm});\n")
    open(os.path.join(OUT, "cq_icons.c"), "w").write("".join(src))
    open(os.path.join(OUT, "cq_icons.h"), "w").write("".join(hdr))
    print("icons:", len(I) * len(SIZES))
