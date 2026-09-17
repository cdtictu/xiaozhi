#!/bin/bash
# Tien ich Xiaozhi: ./xz.sh build | flash | monitor | menuconfig | restore
. "$HOME/xiaozhi/esp-idf/export.sh" > /dev/null
cd "$HOME/xiaozhi/xiaozhi-esp32"

# Chi nhan dung 2 cong cua ESP nay (theo so serial), khong bao gio doan
UART_PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_5AE7100171-if00
JTAG_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:B5:4E:C6:AC:4C-if00
pick_port() {
  if   [ -n "$PORT" ];      then :
  elif [ -e "$UART_PORT" ]; then PORT=$UART_PORT
  elif [ -e "$JTAG_PORT" ]; then PORT=$JTAG_PORT
  else
    # KHONG tu dong lay /dev/ttyACM0: board STM32 cung co the hien ra ten do
    echo "[xz] LOI: khong thay ESP32 (CH343 hoac USB-JTAG). Dung lai de tranh nap nham vao STM32." >&2
    echo "[xz] Neu chac chan dung cong khac: PORT=/dev/ttyACMx $0 $*" >&2
    exit 1
  fi
  echo "[xz] dung cong: $PORT"
}

case "$1" in
  ""|build)   idf.py build ;;          # build khong can cam ESP
  menuconfig) idf.py menuconfig ;;
  restore)
    pick_port "$@"
    echo "Khoi phuc firmware TestData cu vao $PORT..."
    python -m esptool --chip esp32s3 -p "$PORT" write-flash 0x0 \
        "$HOME/xiaozhi/backup/esp2_AC4C_TestData_2MB.bin" ;;
  monitor) pick_port "$@"; idf.py -p "$PORT" monitor ;;
  flash)   pick_port "$@"; idf.py -p "$PORT" flash ;;
  *)       pick_port "$@"; idf.py -p "$PORT" "$@" ;;
esac
