# 1. 针对 ESP32-S3 核心进行编译
/Users/qinshen/go/zephyrproject/.venv/bin/west build -p always -b esp32s3_devkitc/esp32s3/procpu

# 2. 烧录固件到 Tiny 板
/Users/qinshen/go/zephyrproject/.venv/bin/west flash --erase

/Users/qinshen/go/zephyrproject/.venv/bin/west espressif monitor









