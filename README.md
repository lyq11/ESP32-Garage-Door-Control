# ESP32 Garage Door Control

基于 ESP32-S3 与 ESP32-C3 的双节点车库门控制系统。S3 是中控节点，负责身份识别、传感器采集、自动关门逻辑和管理界面；C3 安装在车库电机侧，负责接收 S3 的 ESP-NOW 指令并输出继电器脉冲。

## 系统架构

```text
FP001 人脸/掌静脉模块 ─┐
RS485 门磁/震动传感器 ─┼─> ESP32-S3 中控 ── ESP-NOW ──> ESP32-C3 电机节点 ──> 继电器 ──> 车库门电机
Web 管理台 / 飞书通知 ──┘          <────── ACK ──────
```

| 节点 | 工程目录 | 职责 |
| --- | --- | --- |
| ESP32-S3 中控 | [`firmware/s3_central`](firmware/s3_central) | FP001 身份识别、双路 RS485、门状态判断、自动关门、ESP-NOW 指令、飞书通知、Web API、PC 管理台、OTA、看门狗和 Wi-Fi 恢复 |
| ESP32-C3 车库电机节点 | [`firmware/c3_garage_node`](firmware/c3_garage_node) | 校验 ESP-NOW 指令、继电器脉冲输出、ACK、Web 配置页、OTA、看门狗和 Wi-Fi 恢复 |

## 目录结构

```text
firmware/
├── s3_central/
│   ├── s3_central.ino       # S3 主程序
│   ├── face_fp001.*         # FP001 人脸/掌静脉模块
│   ├── rs485_modbus.*       # 双路 RS485 Modbus
│   ├── garage_espnow.*      # 车库门状态机与 ESP-NOW
│   ├── notify_feishu.*      # 飞书通知
│   ├── web_api.*            # HTTP API
│   ├── ota_update.*         # Arduino OTA
│   └── pc-dashboard/        # PC 端管理页面与本地代理
└── c3_garage_node/
    └── c3_garage_node.ino   # C3 完整固件
```

## 硬件连接

### ESP32-S3 中控

默认引脚定义位于 `firmware/s3_central/config_pins.h`。

| 功能 | 引脚 |
| --- | --- |
| FP001 RX / TX | GPIO 40 / GPIO 41 |
| 早期日志 TX | GPIO 43 |
| RS485-1 TX / RX / DE-RE | GPIO 15 / GPIO 16 / GPIO 6 |
| RS485-2 TX / RX / DE-RE | GPIO 17 / GPIO 18 / GPIO 7 |
| 人脸唤醒输入 | GPIO 8 |

### ESP32-C3 电机节点

| 功能 | 默认值 |
| --- | --- |
| 继电器输出 | GPIO 10 |
| 有效电平 | HIGH |
| 继电器脉冲 | 700 ms，可在网页中设置为 100–5000 ms |

继电器应通过合适的驱动、光耦或隔离模块连接车库门控制器，不要直接用 ESP32 GPIO 驱动继电器线圈。

## 软件环境

- Arduino IDE 2.x，或 Arduino CLI
- Espressif ESP32 Arduino Core 3.3.6
- S3 板卡：`esp32:esp32:esp32s3`
- C3 板卡：`esp32:esp32:esp32c3`
- 固件只使用 ESP32 Arduino Core 自带库
- PC 管理台需要 Node.js 18 或更高版本，不需要第三方 npm 依赖

## 编译

在仓库根目录执行：

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/s3_central
arduino-cli compile --fqbn esp32:esp32:esp32c3 firmware/c3_garage_node
```

也可以分别用 Arduino IDE 打开：

- `firmware/s3_central/s3_central.ino`
- `firmware/c3_garage_node/c3_garage_node.ino`

## 首次配置

建议先刷写 C3，再刷写 S3。

### 1. 配置 C3 电机节点

1. 刷写 `c3_garage_node`。
2. 首次启动会建立 Wi-Fi：`garage-c3-setup`，默认密码 `12345678`。
3. 浏览器打开 `http://192.168.4.1/`。
4. 默认 HTTP Basic Auth 用户名为 `admin`，密码为 `12345678`。
5. 保存家庭 Wi-Fi、继电器脉冲、共享密钥和允许的 S3 MAC 地址。

### 2. 配置 S3 中控

1. 刷写 `s3_central`。
2. 首次启动会建立 Wi-Fi：`centr-setup`，默认密码 `12345678`。
3. 通过设备 API 或 PC 管理台保存家庭 Wi-Fi。
4. 在 Garage 配置中填写 C3 的 Wi-Fi MAC、与 C3 相同的共享密钥，并启用车库控制。

S3 和 C3 必须工作在同一个 2.4 GHz Wi-Fi 信道。两台设备连接同一接入点通常可以自动满足这一要求。

## PC 管理台

S3 本身提供 JSON API；完整管理界面位于 `firmware/s3_central/pc-dashboard`。启动本地代理：

```powershell
cd firmware/s3_central/pc-dashboard
npm start -- --target=http://S3的IP地址
```

然后打开 `http://127.0.0.1:5173/`。管理台包含系统状态、日志、身份识别、人员管理、RS485 传感器、车库门控制和通知配置。

常用 S3 接口：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/status` | 系统、Wi-Fi、内存和时间状态 |
| GET | `/api/logs` | 运行日志 |
| GET | `/api/face/status` | FP001 状态 |
| GET | `/api/garage/status` | 车库门与 ESP-NOW 状态 |
| POST | `/api/garage/trigger` | 触发车库门动作 |
| POST | `/api/garage/config` | 保存 C3 MAC、共享密钥和自动关门参数 |
| GET | `/api/rs485/1/status` | RS485-1 状态 |
| GET | `/api/rs485/2/status` | RS485-2 状态 |

## OTA 更新

设备连接 Wi-Fi 后支持 Arduino OTA。示例：

```powershell
# S3
arduino-cli upload --fqbn esp32:esp32:esp32s3 --protocol network --port S3的IP地址 --build-path 编译输出目录 firmware/s3_central

# C3（password 为设备管理密码）
arduino-cli upload --fqbn esp32:esp32:esp32c3 --protocol network --port C3的IP地址 --upload-field password=设备管理密码 --build-path 编译输出目录 firmware/c3_garage_node
```

## 可靠性设计

- S3 和 C3 均启用任务看门狗。
- Wi-Fi 掉线后持续自动重连；C3 长期断网会同时开放设置 AP。
- C3 的 ESP-NOW 接收回调只负责入队，指令验证和继电器操作在主循环执行。
- C3 继电器命令带冷却时间、序列号检查、可选发送端 MAC 白名单和 ACK。
- C3 将最后接受的序列号持久化到 NVS，重启后仍能阻止旧包重放；管理网页提供受认证保护的序列号清除按钮，更换 S3 MAC 白名单时也会自动清除旧序列号。
- S3 保存 ESP-NOW 序列号，并根据门磁/震动状态执行自动关门状态机。

序列号为 32 位无符号整数。按系统 5 秒冷却时间连续发送，理论上约 681 年才会耗尽，因此没有使用依赖联网和校时稳定性的 NTP 时间作为序列号。更换 S3 中控时，应先在 C3 网页更新 S3 MAC；C3 会自动重置旧中控的序列状态。也可以在 C3 网页的 **ESP-NOW replay protection** 区域手动执行 **Clear old sequence**。

## 安全注意事项

这是能够驱动物理设备的控制系统。部署前至少完成以下事项：

1. 修改两个设置 AP 的默认密码。
2. 修改 C3 默认管理员密码。
3. 为每套设备设置唯一且足够长的共享密钥，并配置双方 MAC。
4. 将设备放在可信的隔离网络中；当前 S3 Web API 和 S3 OTA 没有认证保护。
5. 当前 ESP-NOW 应用层签名使用带共享密钥的 FNV-1a，它不是现代密码学 MAC；高安全性场景应升级为 HMAC-SHA256 或启用 ESP-NOW 链路层加密。
6. 初次测试时断开电机，只观察继电器或使用低压测试负载，确认方向、脉冲时间、门磁状态和自动关门逻辑后再接入实际电机。

仓库不包含实际 Wi-Fi 凭据、飞书 Webhook、已配置的设备 MAC 或 NVS 数据。
