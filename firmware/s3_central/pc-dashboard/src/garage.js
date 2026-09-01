import { api, post } from "./api.js?v=20260628-garage-timing";
import { $, kv, val, zhValue } from "./dom.js?v=20260628-garage-timing";
import { showResult } from "./logs.js?v=20260628-garage-timing";

export async function refreshGarage() {
  const status = await api("/api/garage/status");
  $("garageStatus").innerHTML = kv(status);
  if (status.peerMac && !$("garageMac").value) {
    $("garageMac").value = status.peerMac;
  }
  $("garageChannel").value = status.channel || 11;
  $("garageEnabled").value = status.enabled ? "true" : "false";
  $("garageAutoClose").value = status.autoCloseEnabled ? "true" : "false";
  $("garageSendCooldownMs").value = status.sendCooldownMs ?? $("garageSendCooldownMs").value;
  $("garageOpenStableSeconds").value = status.openStableSeconds ?? $("garageOpenStableSeconds").value;
  $("garageOpenRecheckMs").value = status.openRecheckMs ?? $("garageOpenRecheckMs").value;
  $("garageMovingRecheckMs").value = status.movingRecheckMs ?? $("garageMovingRecheckMs").value;
  $("garagePostTriggerRecheckMs").value = status.postTriggerRecheckMs ?? $("garagePostTriggerRecheckMs").value;
  $("garageMaxAttemptRecheckMs").value = status.maxAttemptRecheckMs ?? $("garageMaxAttemptRecheckMs").value;
  $("garageMaxAutoCloseAttempts").value = status.maxAutoCloseAttempts ?? $("garageMaxAutoCloseAttempts").value;
}

export async function refreshGarageRecords() {
  const data = await api("/api/garage/records");
  const records = data.records || [];
  $("garageRecords").textContent = records.length
    ? records.map((record) => {
      const ack = record.sent ? (record.ackOk ? "C3 已确认" : "等待/未确认") : "未发送";
      const user = record.userId >= 0 ? `用户 ${record.userId}` : "无用户";
      return `[${record.time}] ${zhValue(record.source)} ${user} 门=${zhValue(record.doorState)} ${ack} 原因=${zhValue(record.reason)} seq=${record.seq}`;
    }).join("\n")
    : "暂无开门记录";
}

export function bindGarage() {
  $("btnGarageSave").onclick = async () => {
    try {
      const result = await post("/api/garage/config", {
        mac: val("garageMac"),
        secret: val("garageSecret"),
        channel: val("garageChannel"),
        enabled: val("garageEnabled"),
        autoCloseEnabled: val("garageAutoClose"),
        sendCooldownMs: val("garageSendCooldownMs"),
        openStableSeconds: val("garageOpenStableSeconds"),
        openRecheckMs: val("garageOpenRecheckMs"),
        movingRecheckMs: val("garageMovingRecheckMs"),
        postTriggerRecheckMs: val("garagePostTriggerRecheckMs"),
        maxAttemptRecheckMs: val("garageMaxAttemptRecheckMs"),
        maxAutoCloseAttempts: val("garageMaxAutoCloseAttempts")
      });
      showResult(result);
      setTimeout(refreshGarage, 500);
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnGarageTrigger").onclick = async () => {
    try {
      const result = await post("/api/garage/trigger", {});
      showResult(result);
      setTimeout(refreshGarage, 500);
      setTimeout(refreshGarageRecords, 800);
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnGarageRecords").onclick = async () => {
    try {
      await refreshGarageRecords();
    } catch (error) {
      showResult(String(error));
    }
  };
}
