export const $ = (id) => document.getElementById(id);

export function val(id) {
  return $(id).value;
}

const labelMap = {
  ok: "结果",
  initialized: "已初始化",
  online: "在线",
  hostname: "主机名",
  ip: "IP 地址",
  mac: "MAC 地址",
  uptime: "运行时间",
  uptimeMs: "运行时间",
  heap: "剩余内存",
  freeHeap: "剩余内存",
  minFreeHeap: "历史最低剩余堆",
  largestFreeBlock: "最大连续内存块",
  wifi: "WiFi",
  ssid: "SSID",
  rssi: "WiFi 信号 dBm",
  signalPercent: "WiFi 信号强度 %",
  wifiChannel: "WiFi 信道",
  channel: "信道",
  ota: "OTA",
  timeReady: "NTP 时间已同步",
  localTime: "当前北京时间",
  nextScheduledRestart: "下次定时重启",
  baud: "波特率",
  rxPin: "RX 引脚",
  txPin: "TX 引脚",
  rxBytes: "接收字节",
  txBytes: "发送字节",
  frameCount: "正常帧数",
  badFrameCount: "错误帧数",
  frames: "帧统计",
  pendingMid: "等待命令",
  moduleStatus: "模块状态",
  mode: "工作模式",
  autoVerifyEnabled: "自动识别",
  autoVerifyActive: "识别窗口激活",
  autoVerifyWindowUntilMs: "识别窗口截止",
  lastResultCode: "上次结果码",
  lastResultText: "上次结果",
  lastUserId: "上次用户 ID",
  lastUserName: "上次用户名",
  lastFaceState: "上次人脸状态",
  lastFaceStateText: "上次人脸状态",
  faceState: "人脸状态",
  userCount: "用户数量",
  faceEnroll5Active: "五步录入中",
  faceEnroll5Step: "五步录入步骤",
  rxAvailable: "接收缓存",
  led: "LED 状态",
  green: "绿灯",
  red: "红灯",
  white: "白灯",
  enroll5: "五步录入",
  enabled: "启用",
  hardwareEnabled: "硬件接口已初始化",
  espNowReady: "ESP-NOW 就绪",
  peerConfigured: "C3 已配置",
  peerMac: "C3 MAC",
  autoCloseEnabled: "自动关门",
  sendCooldownMs: "触发冷却 ms",
  openStableSeconds: "开门稳定等待 s",
  openRecheckMs: "开门复查间隔 ms",
  movingRecheckMs: "运动复查间隔 ms",
  postTriggerRecheckMs: "关门后复查 ms",
  maxAttemptRecheckMs: "保护后复查 ms",
  maxAutoCloseAttempts: "最大自动关门次数",
  limitMode: "门限模式",
  singleLimitPort: "单门限端口",
  upperLimitActive: "上门限已触发",
  lowerLimitActive: "下门限已触发",
  motionDirection: "运动方向",
  travelElapsedSeconds: "本次行程秒数",
  travelTimeoutSeconds: "行程超时秒数",
  travelTimedOut: "行程已超时",
  doorState: "门状态",
  logicStage: "逻辑阶段",
  nextLogicMs: "下次检查时间",
  autoCloseAttempts: "自动关门次数",
  lastSendMs: "上次发送时间",
  lastSendOk: "上次发送成功",
  lastAckMs: "上次应答时间",
  lastAckSeq: "上次应答序号",
  lastAckOk: "上次应答成功",
  lastReason: "上次原因",
  webhookConfigured: "Webhook 已配置",
  cooldownSec: "冷却秒数",
  maxAlerts: "最大发送条数",
  lastAlertMs: "上次告警时间",
  sentCount: "发送次数",
  failCount: "失败次数",
  suppressedCount: "抑制次数",
  name: "名称",
  port: "端口",
  addr: "地址",
  nodeAddr: "节点地址",
  reg: "寄存器",
  value: "数值",
  result: "执行结果",
  config: "配置",
  raw: "原始值",
  values: "读取值",
  registers: "寄存器",
  lastReadOk: "最近读取成功",
  lastReadMs: "最近读取时间",
  successCount: "成功次数",
  savedConfig: "已保存配置",
  savedConfigApplied: "配置已应用",
  savedConfigAttempts: "配置恢复次数",
  role: "用途",
  deRePin: "DE/RE 引脚",
  hallClosed: "霍尔已闭合",
  hallValue: "霍尔状态",
  lastSeenHall: "上次霍尔状态",
  doorVibration: "门体震动/运行",
  vibrationCounter: "震动累计计数",
  lastSeenCounter: "上次计数",
  singlePort: "单门限端口编号",
  singlePortName: "单门限端口",
  upperPort: "上门限端口编号",
  upperPortName: "上门限端口",
  lowerPort: "下门限端口编号",
  lowerPortName: "下门限端口",
  limitActive: "门限已触发",
  motionDetected: "检测到运动",
  modbusAddr: "Modbus 地址",
  magHoldMs: "霍尔吸合确认 ms",
  magReleaseMs: "霍尔释放确认 ms",
  vibrationWindowMs: "震动计数窗口 ms",
  vibrationThreshold: "震动触发阈值",
  runHoldMs: "运行保持 ms",
  ledEnabled: "LED 指示",
  rs485_1: "RS485-1",
  rs485_2: "RS485-2"
};

const valueMap = {
  true: "是",
  false: "否",
  null: "空",
  undefined: "未定义",
  SUCCESS: "成功",
  NONE: "无",
  IDLE: "空闲",
  AUTO_VERIFY: "自动识别",
  COOLDOWN: "冷却中",
  STANDBY: "待机",
  UNKNOWN_FACE_STATE: "未知人脸状态",
  NO_FACE: "未检测到人脸",
  HAS_FACE: "检测到人脸",
  HAND_NORMAL: "检测到掌静脉",
  HAND_TOO_FAR: "手掌距离太远",
  HAND_TOO_CLOSE: "手掌距离太近",
  HAND_TOO_UP: "手掌太靠上",
  HAND_TOO_DOWN: "手掌太靠下",
  HAND_TOO_LEFT: "手掌太靠左",
  HAND_TOO_RIGHT: "手掌太靠右",
  HAND: "掌静脉",
  FRONT: "正向人脸",
  RIGHT: "右向人脸",
  LEFT: "左向人脸",
  DOWN: "下向人脸",
  UP: "上向人脸",
  TIMEOUT: "超时",
  HAND_VERIFY_FAIL: "掌静脉验证失败",
  HAND_ALREADY_ENROLLED: "掌静脉重复",
  FACE_ALREADY_ENROLLED: "人脸已录入",
  INVALID_PARAM: "参数无效",
  READ_FILE_FAIL: "读文件失败",
  WRITE_FILE_FAIL: "写文件失败",
  NO_ENCRYPT: "通信未加密",
  NO_RGBIMAGE: "RGB 图像未就绪",
  UNKNOWN: "未知",
  OPEN: "已开门",
  CLOSED: "已关门",
  MOVING: "运动中",
  STOPPED: "中途停止",
  OPENING: "正在开门",
  CLOSING: "正在关门",
  SINGLE: "单门限",
  DUAL: "上下门限",
  CONFLICT: "传感器冲突",
  upper_limit: "上门限（全开）",
  lower_limit: "下门限（全关）",
  single_limit: "单门限（全关）",
  auxiliary: "辅助采样",
  disabled: "已关闭（不轮询）",
  manual_api: "手动 API",
  face_verify: "人脸识别",
  face_success: "人脸识别成功",
  FACE_OK: "人脸识别成功",
  FACE_EYE_CLOSED_OK: "人脸识别成功（闭眼）",
  HAND_OK: "掌静脉识别成功",
  auto_close: "自动关门",
  trigger: "触发",
  not_configured: "未配置",
  cooldown: "冷却中",
  send_failed: "发送失败",
  door_not_closed: "门未关闭",
  unsafe_door_state: "门状态不安全",
  ACCEPTED: "已受理",
  FACE_ACTIVE: "识别窗口已开启",
  RECENT_GARAGE_TRIGGER: "近期已触发车库",
  DOOR_NOT_CLOSED: "车库门未关闭",
  COOLDOWN: "唤醒冷却中",
  DEBOUNCE_REJECTED: "消抖未通过",
  arduino_ota: "ArduinoOTA",
  outside_vibration: "外侧二合一（仅采样）",
  door_state: "门状态"
};

export function zhLabel(key) {
  return labelMap[key] || key;
}

export function zhValue(value) {
  if (typeof value === "boolean") {
    return value ? "是" : "否";
  }
  if (value === null) {
    return "空";
  }
  if (Array.isArray(value)) {
    return value.map(zhValue).join(", ");
  }
  if (typeof value === "object") {
    return kv(value);
  }
  const text = String(value);
  return valueMap[text] || text;
}

export function kv(obj) {
  return Object.keys(obj)
    .map((key) => `<div class="kv"><b>${zhLabel(key)}</b><span>${zhValue(obj[key])}</span></div>`)
    .join("");
}

export function badge(text, cls) {
  return `<span class="badge ${cls}">${text}</span>`;
}

export function setDisabled(ids, disabled) {
  ids.forEach((id) => {
    $(id).disabled = disabled;
  });
}

export function setActive(id, active) {
  $(id).classList.toggle("active", !!active);
}

export function formBody(obj) {
  const body = new URLSearchParams();
  Object.keys(obj).forEach((key) => body.append(key, obj[key]));
  return body;
}
