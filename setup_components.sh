#!/bin/bash
# 拉 esp-iot-solution master 分支组件 (含 iot_usbh_rndis / iot_eth)
# 用 codeload zip 下载, 避开 git clone 大仓库卡死
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
COMPONENTS_DIR="$SCRIPT_DIR/components"
IOT_SOLUTION_ZIP="https://codeload.github.com/espressif/esp-iot-solution/zip/refs/heads/master"

# 杀卡死的 git 进程 (残留)
pkill -9 -f "git clone.*esp-iot-solution" 2>/dev/null || true
sleep 1

rm -rf "$COMPONENTS_DIR"
mkdir -p "$COMPONENTS_DIR"
TMP_DIR=$(mktemp -d)
echo "[INFO] 下载 esp-iot-solution master zip..."
curl -L --max-time 120 -o "$TMP_DIR/repo.zip" "$IOT_SOLUTION_ZIP" 2>&1 | tail -1

if [ ! -s "$TMP_DIR/repo.zip" ]; then
    echo "[ERROR] zip 下载失败 (空文件)"; rm -rf "$TMP_DIR"; exit 1
fi

echo "[INFO] 解压到 $TMP_DIR ..."
unzip -q "$TMP_DIR/repo.zip" -d "$TMP_DIR"
ROOT=$(find "$TMP_DIR" -maxdepth 1 -type d -name "esp-iot-solution-*" | head -1)
if [ -z "$ROOT" ]; then
    echo "[ERROR] 解压后找不到 esp-iot-solution-* 目录"
    rm -rf "$TMP_DIR"; exit 1
fi
echo "[INFO] 仓库根: $ROOT"

# 拷贝子组件 (用绝对路径, 不用 $OLDPWD)
for sub in iot_eth usb/iot_usbh usb/iot_usbh_cdc usb/iot_usbh_rndis; do
    SRC="$ROOT/components/$sub"
    DST="$COMPONENTS_DIR/$sub"
    if [ -d "$SRC" ]; then
        mkdir -p "$(dirname "$DST")"
        cp -r "$SRC" "$DST"
        echo "  [CP] $sub"
    else
        echo "  [SKIP] $sub (not in master)"
    fi
done

rm -rf "$TMP_DIR"

echo ""
echo "[CHECK] 验证必需头文件..."
MISSING=0
for hdr in iot_usbh_rndis.h iot_usbh_cdc.h iot_usbh.h iot_eth.h iot_eth_netif_glue.h; do
    FOUND=$(find "$COMPONENTS_DIR" -name "$hdr" 2>/dev/null | head -1)
    if [ -n "$FOUND" ]; then
        echo "  [OK]   $hdr -> $FOUND"
    else
        echo "  [MISS] $hdr"
        MISSING=$((MISSING+1))
    fi
done

echo ""
if [ $MISSING -gt 0 ]; then
    echo "[FAIL] $MISSING 个头文件缺失, 无法编译"
    exit 1
fi
echo "============================================================"
echo "  全部就绪! 现在跑: pio run"
echo "============================================================"
