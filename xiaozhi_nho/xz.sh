#!/bin/bash
# Tien ich cho ban NHO (ESP32 thuong): ./xz.sh build | flash | monitor | menuconfig
# Ban lon dung ~/xiaozhi/xz.sh, hai ban khong dung chung thu muc build.
. "$HOME/xiaozhi/esp-idf/export.sh" > /dev/null
cd "$HOME/xiaozhi/xiaozhi_nho/xiaozhi-esp32" || exit 1

# Cong cua board LON (ESP32-S3, chip CH343) - KHONG bao gio nap vao day
BIG_BOARD=/dev/serial/by-id/usb-1a86_USB_Single_Serial_5AE7100171-if00

pick_port() {
  if [ -n "$PORT" ]; then
    echo "[nho] dung cong chi dinh: $PORT" >&2
    return
  fi
  for p in /dev/serial/by-id/*; do
    [ -e "$p" ] || continue
    [ "$(readlink -f "$p")" = "$(readlink -f "$BIG_BOARD" 2>/dev/null)" ] && continue
    PORT="$p"
    echo "[nho] dung cong: $PORT" >&2
    return
  done
  echo "[nho] LOI: khong thay ESP32 nao (ngoai board lon)." >&2
  echo "[nho] Cam board nho vao, hoac chi dinh: PORT=/dev/ttyUSB0 $0 $*" >&2
  exit 1
}

case "$1" in
  ""|build)   idf.py build ;;
  menuconfig) idf.py menuconfig ;;
  monitor)    pick_port "$@"; idf.py -p "$PORT" monitor ;;
  flash)      pick_port "$@"; idf.py -p "$PORT" flash ;;
  *)          pick_port "$@"; idf.py -p "$PORT" "$@" ;;
esac
