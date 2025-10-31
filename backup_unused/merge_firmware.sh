#!/bin/bash

BUILD_DIR="build"
FINAL_BIN="firmware_final.bin"
FLASHER_JSON="$BUILD_DIR/flasher_args.json"

# Kiểm tra file tồn tại
if [ ! -f "$FLASHER_JSON" ]; then
    echo "❌ Không tìm thấy $FLASHER_JSON — hãy chạy 'idf.py build' trước."
    exit 1
fi

# Lấy chip từ extra_esptool_args (nếu có), mặc định esp32s3
CHIP=$(jq -r '.extra_esptool_args.chip // "esp32s3"' "$FLASHER_JSON")

# Tạo chuỗi lệnh merge
MERGE_CMD="esptool.py --chip $CHIP merge_bin -o $FINAL_BIN"

# Đọc từng cặp offset - file từ flash_files
while read -r offset file; do
    MERGE_CMD="$MERGE_CMD $offset $BUILD_DIR/$file"
done < <(jq -r '.flash_files | to_entries[] | "\(.key) \(.value)"' "$FLASHER_JSON")

# In ra lệnh kiểm tra
echo "⚡ Đang thực thi:"
echo "$MERGE_CMD"

# Chạy merge
eval "$MERGE_CMD"

# Kiểm tra kết quả
if [ -f "$FINAL_BIN" ]; then
    echo "✅ Đã tạo file firmware hợp nhất: $FINAL_BIN"
    ls -lh "$FINAL_BIN"
else
    echo "❌ Merge thất bại."
    exit 1
fi

