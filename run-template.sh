#!/bin/bash

# 检查是否提供了文件夹参数
if [ $# -ne 1 ]; then
    echo "Usage: $0 <directory>"
    exit 1
fi

# 获取传入的文件夹路径
dir=$1

# 检查文件夹是否存在
if [ ! -d "$dir" ]; then
    echo "Error: Directory $dir does not exist."
    exit 1
fi

LOGNAME=$dir/log
rm $LOGNAME


# 遍历子文件夹
for subdir in "$dir"/*; do
    # 检查是否是目录
    if [ -d "$subdir" ]; then
        # 提取目录名中的数字
        basename=$(basename "$subdir")
        # 使用正则表达式提取数字
        if [[ "$basename" =~ ([0-9]+) ]]; then
            # 提取到的数字会存储在 ${BASH_REMATCH[1]} 中
            numbers="${BASH_REMATCH[1]}"
            
            # 打印提取的数字（仅供调试）
            echo "Found number: $numbers"
            
            # 执行 make 命令和其他操作
            make clean-template
            make template NThread=$numbers NCore=$numbers -j
            ./performance/replay_two_level $subdir>> $LOGNAME
            ./performance/replay_two_level_st $subdir >> $LOGNAME
            ./performance/replay_three_level $subdir>> $LOGNAME
            ./performance/replay_three_level_st $subdir>> $LOGNAME
        else
            echo "No numbers found in $basename"
        fi
    fi
done

