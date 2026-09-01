import { api, post } from "./api.js?v=20260829-io8-wake";
import { $, badge, kv, setActive, setDisabled, val, zhValue } from "./dom.js?v=20260829-io8-wake";
import { showResult } from "./logs.js?v=20260829-io8-wake";

let faceState = {};

function verifyTypeText(status) {
  const text = status.lastUnlockStatusText || "NONE";
  if (text === "FACE_OK") return "人脸";
  if (text === "FACE_EYE_CLOSED_OK") return "人脸（闭眼）";
  if (text === "HAND_OK") return "掌静脉";
  if (text === "NONE") return "无";
  return zhValue(text);
}

function enroll5Prompt(status) {
  if (!status.faceEnroll5Active) return "关闭";
  const map = {
    FRONT: "请正对摄像头",
    RIGHT: "请向右转头",
    LEFT: "请向左转头",
    DOWN: "请向下低头",
    UP: "请向上抬头"
  };
  const direction = map[status.faceEnroll5DirectionText] || "请按模块提示调整";
  return `第 ${status.faceEnroll5Step} 步：${direction}`;
}

function renderFace(status) {
  faceState = status;
  const online = !!status.online;
  const busy = !!status.pendingMid || !["IDLE", "AUTO_VERIFY", "COOLDOWN"].includes(status.mode);
  const auto = !!status.autoVerifyEnabled;
  const resultOk = status.lastResultText === "SUCCESS";
  const verifyType = verifyTypeText(status);
  const wake = status.wakeInput || {};

  $("faceSummary").innerHTML = [
    badge(online ? "在线" : "离线", online ? "ok" : "bad"),
    badge(auto ? "自动识别已开启" : "自动识别已关闭", auto ? "ok" : "idle"),
    badge(busy ? `忙碌：${zhValue(status.mode)}` : `模式：${zhValue(status.mode)}`, busy ? "warn" : "idle"),
    badge(`模块：${zhValue(status.moduleStatus)}`, status.moduleStatus === "STANDBY" ? "ok" : "warn"),
    badge(`上次结果：${zhValue(status.lastResultText)}`, resultOk ? "ok" : status.lastResultText === "NONE" ? "idle" : "bad"),
    badge(`识别类型：${verifyType}`, verifyType === "掌静脉" ? "ok" : "idle"),
    badge(`IO${wake.pin ?? 8}：${wake.active ? "低电平触发" : "高电平待机"}`, wake.active ? "warn" : "ok")
  ].join("");

  $("faceStatus").innerHTML = kv({
    "RX 引脚": status.rxPin,
    "TX 引脚": status.txPin,
    "接收字节": status.rxBytes,
    "发送字节": status.txBytes,
    "帧统计": `${status.frameCount} 正常 / ${status.badFrameCount} 错误`,
    "人脸用户": status.userCount || 0,
    "掌静脉用户": status.handUserCount || 0,
    "上次用户 ID": status.lastUserId,
    "上次用户名": status.lastUserName || "",
    "上次识别类型": verifyType,
    "上次录入方向": zhValue(status.lastEnrollDirectionText || "NONE"),
    "unlockStatus": status.lastUnlockStatusText || "NONE",
    "faceState": zhValue(status.lastFaceStateText),
    "等待命令": status.pendingMid,
    "五步录入": enroll5Prompt(status),
    "唤醒来源": `IO${wake.pin ?? 8} 低有效（RS485-1 仅采样）`,
    "唤醒输入电平": wake.level ?? "未知",
    "唤醒请求次数": wake.requestCount ?? 0,
    "成功唤醒次数": wake.acceptedCount ?? 0,
    "上次唤醒判定": zhValue(wake.lastResult || "NONE"),
    "上次唤醒事件 ms": wake.lastEventMs ?? 0
  });

  const userText = $("faceUsers").textContent.trim();
  const totalUsers = (status.userCount || 0) + (status.handUserCount || 0);
  if (totalUsers > 0 && (!userText || userText === "暂无用户")) {
    $("faceUsers").textContent = `模块报告有人脸 ${status.userCount || 0} 个、掌静脉 ${status.handUserCount || 0} 个，点击“刷新用户”读取 ID。`;
  }

  setDisabled(["btnVerify", "btnAutoOn"], !online || busy || auto);
  setDisabled(["btnAutoOff"], !online || !auto);
  setDisabled(["btnFaceReset"], !online);
  setDisabled([
    "btnGreenOn", "btnGreenOff", "btnRedOn", "btnRedOff", "btnWhiteOn", "btnWhiteOff"
  ], !online || busy);

  const led = status.led || {};
  setActive("btnGreenOn", !!led.green);
  setActive("btnGreenOff", !led.green);
  setActive("btnRedOn", !!led.red);
  setActive("btnRedOff", !led.red);
  setActive("btnWhiteOn", !!led.white);
  setActive("btnWhiteOff", !led.white);
}

async function facePost(path, body) {
  try {
    const result = await post(path, body);
    showResult(result);
    setTimeout(refreshFace, 500);
  } catch (error) {
    showResult(String(error));
  }
}

function enrollBody() {
  return {
    name: val("enrollName"),
    timeout: val("enrollTimeout"),
    admin: val("admin")
  };
}

function handEnrollBody() {
  const body = enrollBody();
  body.timeout = Math.max(Number(body.timeout) || 0, 120);
  return body;
}

function renderUsers(data) {
  const faceIds = data.faceIds || data.ids || [];
  const handIds = data.handIds || [];
  const parts = [];
  if (faceIds.length) {
    parts.push(`人脸用户：${faceIds.map((id) => `ID ${id}`).join(", ")}`);
  } else if ((data.faceCount || data.count || 0) > 0) {
    parts.push(`人脸用户：模块报告 ${data.faceCount || data.count} 个，但本次没有返回 ID`);
  }
  if (handIds.length) {
    parts.push(`掌静脉用户：${handIds.map((id) => `ID ${id}`).join(", ")}`);
  } else if ((data.handCount || 0) > 0) {
    parts.push(`掌静脉用户：模块报告 ${data.handCount} 个，但本次没有返回 ID`);
  }
  $("faceUsers").textContent = parts.length ? parts.join("\n") : "暂无用户";
}

export async function refreshFace() {
  renderFace(await api("/api/face/status"));
}

export async function refreshUsers() {
  try {
    renderUsers(await api("/api/face/users"));
    setTimeout(async () => {
      try {
        renderUsers(await api("/api/face/users?cached=1"));
        refreshFace();
      } catch (error) {
        showResult(String(error));
      }
    }, 1200);
  } catch (error) {
    showResult(String(error));
  }
}

export function bindFace() {
  $("btnVerify").onclick = () => facePost("/api/face/verify", { timeout: 10 });
  $("btnAutoOn").onclick = () => facePost("/api/face/auto/start", {});
  $("btnAutoOff").onclick = () => facePost("/api/face/auto/stop", {});
  $("btnFaceReset").onclick = () => facePost("/api/face/reset", {});
  $("btnGreenOn").onclick = () => facePost("/api/face/led", { color: "green", on: true });
  $("btnGreenOff").onclick = () => facePost("/api/face/led", { color: "green", on: false });
  $("btnRedOn").onclick = () => facePost("/api/face/led", { color: "red", on: true });
  $("btnRedOff").onclick = () => facePost("/api/face/led", { color: "red", on: false });
  $("btnWhiteOn").onclick = () => facePost("/api/face/led", { color: "white", on: true });
  $("btnWhiteOff").onclick = () => facePost("/api/face/led", { color: "white", on: false });
  $("btnEnrollFace").onclick = () => facePost("/api/face/enroll", enrollBody());
  $("btnEnrollFace5").onclick = () => facePost("/api/face/enroll5", enrollBody());
  $("btnEnrollHand").onclick = () => facePost("/api/face/enroll-hand", handEnrollBody());
  $("btnRefreshUsers").onclick = refreshUsers;
  $("btnDeleteUser").onclick = () => facePost("/api/face/users/delete", { id: val("deleteUserId") });
  $("btnDeleteAllUsers").onclick = () => facePost("/api/face/users/delete-all", {});
}

export function getFaceState() {
  return faceState;
}
