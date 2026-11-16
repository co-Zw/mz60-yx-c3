#!/bin/bash

# 检查是否提供了type参数
if [ $# -eq 0 ]; then
    echo "Usage: $0 <type>"
    echo "Example: $0 zuowei-c3-realtime-lcd"
    exit 1
fi

# 获取传入的type参数
TYPE=$1

# 检查jq是否存在
if ! command -v jq &> /dev/null; then
    echo "Error: jq is not installed. Please install jq to use this script."
    echo "Ubuntu/Debian: sudo apt-get install jq"
    echo "macOS: brew install jq"
    exit 1
fi

# 执行curl请求并解析JSON响应
RESPONSE=$(curl -s -X POST \
  "http://xiaozhi.doit.am/ota/ota.php" \
  -H "Accept: application/json" \
  -H "Content-Type: application/json" \
  -d "{\"board\":{\"type\":\"$TYPE\"}}")

# 检查是否有有效的响应
if [ -z "$RESPONSE" ]; then
    echo "Error: No response from server"
    exit 1
fi

# 提取版本号和URL
VERSION=$(echo "$RESPONSE" | jq -r '.firmware.version')
URL=$(echo "$RESPONSE" | jq -r '.firmware.url')

# 检查是否成功获取到版本号和URL
if [ "$VERSION" = "null" ] || [ "$URL" = "null" ]; then
    # echo "Error: Failed to parse firmware information from response"
    # echo "Response: $RESPONSE"
    exit 1
fi

# 输出结果
echo "$1, $VERSION, $URL"