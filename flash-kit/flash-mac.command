#!/bin/bash
# 重师大 AI 工牌 · 一键刷机（Mac）
#
# 用法（在“终端”里，或让 WorkBuddy 帮你运行）：
#   bash flash-mac.command            第一次刷：先备份原厂固件，再刷重师大固件
#   bash flash-mac.command --update   以后升级：只换程序，保留 Wi-Fi 和登记信息
#   bash flash-mac.command --check    只检查工牌能不能连上，什么都不写
#
# 这个脚本只做三件事：读芯片信息、备份、写入固件。不会执行任何“烧熔丝”（espefuse）类的永久操作。
# 最后一行会输出 RESULT=OK 或 RESULT=FAIL，方便 WorkBuddy 判断结果。
set -u
cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"
MODE="${1:-}"

say()  { echo "【工牌】$*"; }
fail() { echo ""; echo "【没成功】$*"; echo "RESULT=FAIL"; exit 1; }

# ---- 1. 准备刷机工具
if [ "$(uname -m)" = "arm64" ]; then T="$HERE/tools/esptool-macos-arm64/esptool"; else T="$HERE/tools/esptool-macos-amd64/esptool"; fi
[ -f "$T" ] || fail "找不到刷机工具 $T，安装包可能没解压完整，请重新解压。"
xattr -dr com.apple.quarantine "$HERE" 2>/dev/null   # 去掉“来自互联网”的标记，否则 macOS 会拦住工具
chmod +x "$T" "$HERE"/*.command 2>/dev/null
for f in bootloader.bin partition-table.bin cqnu-badge.bin; do
  [ -f "$HERE/firmware/$f" ] || fail "固件文件 firmware/$f 不见了，请重新解压安装包。"
done

# ---- 2. 找工牌
PORTS=$(ls /dev/cu.usbmodem* 2>/dev/null)
N=$(echo "$PORTS" | grep -c usbmodem)
if [ "$N" -eq 0 ]; then
  fail "没找到工牌。请检查：① 用能传数据的 USB-C 线（有的线只能充电）② 线插紧 ③ 按一下工牌侧面的键让它亮屏，再运行一次。"
fi
if [ "$N" -gt 1 ]; then
  say "电脑上插了 $N 个设备：$PORTS"
  fail "请只插一张工牌，拔掉其他 USB 设备后再运行。"
fi
PORT="$PORTS"
say "找到设备：$PORT"

ET() { "$T" --chip esp32c3 -p "$PORT" "$@"; }

# ---- 3. 确认是工牌，读出它的编号（MAC）
INFO=$(ET flash-id 2>&1) || { echo "$INFO" | tail -5; fail "连不上工牌。拔掉重插、让工牌亮屏后再试一次。"; }
MAC=$(echo "$INFO" | grep -m1 -i "^MAC:" | awk '{print $2}' | tr -d ':' | tr 'a-f' 'A-F')
SIZE=$(echo "$INFO" | grep -m1 -i "flash size" | sed 's/.*: *//')
[ -n "$MAC" ] || { echo "$INFO" | tail -8; fail "读不到工牌编号。"; }
say "工牌编号 $MAC，存储容量 $SIZE"
case "$SIZE" in 8MB*) ;; *) fail "这块板子的存储容量是 $SIZE，不是 8MB，不是我们发的工牌型号，停止刷机。";; esac

SEC=$(ET get-security-info 2>&1)
if echo "$SEC" | grep -qiE "flash encryption: *enabled|secure boot: *enabled|Flash encryption: *on"; then
  fail "这张工牌开启了加密保护，不能刷第三方固件。请把工牌交给老师。"
fi

if [ "$MODE" = "--check" ]; then
  say "检查通过：工牌连接正常，可以刷机。"
  echo "RESULT=OK"; exit 0
fi

# ---- 4. 第一次刷：先备份原厂固件（每张工牌只备份一次，已经有备份就跳过）
mkdir -p "$HERE/backup"
BK="$HERE/backup/factory-$MAC.bin"
if [ "$MODE" != "--update" ]; then
  if [ -s "$BK" ]; then
    say "已经有这张工牌的备份：backup/factory-$MAC.bin，跳过备份。"
  else
    say "正在备份原厂固件（约 2–4 分钟，别拔线）…"
    ET read-flash 0 0x800000 "$BK.part" || { rm -f "$BK.part"; fail "备份没成功，为了安全没有继续刷机。拔掉重插再试一次。"; }
    mv "$BK.part" "$BK"
    say "备份好了：backup/factory-$MAC.bin（想换回原厂固件时用 restore-mac.command）"
  fi
fi

# ---- 5. 写入重师大固件
FW="$HERE/firmware"
if [ "$MODE" = "--update" ]; then
  say "正在升级程序（保留 Wi-Fi 和登记信息，约 1 分钟）…"
  ET write-flash -z 0x10000 "$FW/cqnu-badge.bin" || fail "写入失败。拔掉重插再运行一次；工牌不会坏。"
else
  say "正在刷入重师大固件（约 1 分钟，别拔线）…"
  ET write-flash -z 0x0 "$FW/bootloader.bin" 0x8000 "$FW/partition-table.bin" 0x10000 "$FW/cqnu-badge.bin" \
    || fail "写入失败。拔掉重插再运行一次；工牌不会坏。"
fi

say "刷好了！工牌会自动重启，屏幕上出现重师大开机画面就成功了。"
say "接下来：用手机“配网小程序”给工牌连 Wi-Fi（2.4G），再按屏幕提示扫码登记。"
echo "RESULT=OK"
