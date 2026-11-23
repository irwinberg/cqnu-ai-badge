#!/bin/bash
# 重师大 AI 工牌 · 恢复（Mac）
#   bash restore-mac.command            换回原厂固件（用第一次刷机时自动备份的 backup/factory-编号.bin）
#   bash restore-mac.command --cqnu     自己改的固件起不来时，重新刷回标准的重师大固件（保留 Wi-Fi 和登记信息）
set -u
cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"
MODE="${1:-}"
say()  { echo "【工牌】$*"; }
fail() { echo ""; echo "【没成功】$*"; echo "RESULT=FAIL"; exit 1; }

if [ "$(uname -m)" = "arm64" ]; then T="$HERE/tools/esptool-macos-arm64/esptool"; else T="$HERE/tools/esptool-macos-amd64/esptool"; fi
xattr -dr com.apple.quarantine "$HERE" 2>/dev/null
chmod +x "$T" 2>/dev/null
PORTS=$(ls /dev/cu.usbmodem* 2>/dev/null)
N=$(echo "$PORTS" | grep -c usbmodem)
[ "$N" -eq 1 ] || fail "请只插一张工牌（现在找到 $N 个设备）。线要能传数据，工牌要亮屏。"
PORT="$PORTS"
ET() { "$T" --chip esp32c3 -p "$PORT" "$@"; }

if [ "$MODE" = "--cqnu" ]; then
  say "正在刷回标准重师大固件…"
  ET write-flash -z 0x0 firmware/bootloader.bin 0x8000 firmware/partition-table.bin 0x10000 firmware/cqnu-badge.bin || fail "写入失败，拔掉重插再试。"
  say "好了，工牌会自动重启。"; echo "RESULT=OK"; exit 0
fi

INFO=$(ET flash-id 2>&1) || fail "连不上工牌，拔掉重插再试。"
MAC=$(echo "$INFO" | grep -m1 -i "^MAC:" | awk '{print $2}' | tr -d ':' | tr 'a-f' 'A-F')
BK="$HERE/backup/factory-$MAC.bin"
[ -s "$BK" ] || fail "没找到这张工牌（编号 $MAC）的原厂备份 backup/factory-$MAC.bin。备份只在用这个安装包第一次刷机时生成。"
say "正在恢复原厂固件（约 3 分钟，别拔线）…"
ET write-flash 0x0 "$BK" || fail "恢复失败，拔掉重插再试。"
say "恢复好了，工牌会自动重启成原厂固件。"
echo "RESULT=OK"
