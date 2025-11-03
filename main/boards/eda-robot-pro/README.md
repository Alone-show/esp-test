# EDA Robot Pro - 机器狗配置

## 概述

EDA Robot Pro 是一个基于ESP32-S3的四足机器狗配置，具有以下特性：

- 四个舵机控制四条腿（左前腿、左后腿、右前腿、右后腿）
- SSD1306 128x64 OLED显示屏
- 音频输入输出支持
- 电池电量监控
- MCP协议控制接口

## 硬件配置

### 舵机连接
- 左前腿: GPIO17
- 左后腿: GPIO18  
- 右前腿: GPIO8
- 右后腿: GPIO12

### 显示屏 (SSD1306 128x64)
- SDA: GPIO41
- SCL: GPIO42

### 音频
- 麦克风 WS: GPIO4
- 麦克风 SCK: GPIO5
- 麦克风 DIN: GPIO6
- 扬声器 DOUT: GPIO7
- 扬声器 BCLK: GPIO15
- 扬声器 LRCK: GPIO16

### 按钮
- 启动按钮: GPIO0
- 触摸按钮: GPIO47
- 音量+: GPIO40
- 音量-: GPIO39

### 其他
- 内置LED: GPIO48
- 电源检测: GPIO21

## MCP控制接口

### 基础动作
- `self.dog.walk` - 行走
- `self.dog.turn` - 转身
- `self.dog.sit` - 坐下
- `self.dog.stand` - 站立
- `self.dog.stretch` - 伸展
- `self.dog.shake` - 摇摆

### 单腿控制
- `self.dog.lift_left_front_leg` - 抬起左前腿
- `self.dog.lift_left_rear_leg` - 抬起左后腿
- `self.dog.lift_right_front_leg` - 抬起右前腿
- `self.dog.lift_right_rear_leg` - 抬起右后腿

### 系统控制
- `self.dog.stop` - 立即停止
- `self.dog.set_trim` - 校准舵机
- `self.dog.get_trims` - 获取微调设置
- `self.dog.get_status` - 获取状态
- `self.battery.get_level` - 获取电池状态

## 编译配置

目标芯片: ESP32-S3
分区表: 16MB
OLED配置: SSD1306 128x64

## 版本信息

当前版本: 1.0.0