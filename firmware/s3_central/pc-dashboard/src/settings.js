import { api, post } from "./api.js?v=20260626-rs485-config";
import { $, kv, val } from "./dom.js?v=20260626-rs485-config";
import { showResult } from "./logs.js?v=20260626-rs485-config";

export async function refreshSettings() {
  const notify = await api("/api/notify/status");
  $("notifyStatus").innerHTML = kv(notify);
  $("notifyEnabled").value = notify.enabled ? "true" : "false";
  $("notifyCooldown").value = notify.cooldownSec || 300;
  $("notifyMaxAlerts").value = notify.maxAlerts ?? 10;
}

export function bindSettings() {
  $("btnSaveWifi").onclick = async () => {
    try {
      showResult(await post("/api/wifi/save", { ssid: val("ssid"), pass: val("pass") }));
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnClearWifi").onclick = async () => {
    try {
      showResult(await post("/api/wifi/clear", {}));
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnSaveNotify").onclick = async () => {
    try {
      showResult(await post("/api/notify/config", {
        enabled: val("notifyEnabled"),
        webhook: val("feishuWebhook"),
        cooldownSec: val("notifyCooldown"),
        maxAlerts: val("notifyMaxAlerts")
      }));
      setTimeout(refreshSettings, 500);
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnTestNotify").onclick = async () => {
    try {
      showResult(await post("/api/notify/test", {}));
      setTimeout(refreshSettings, 500);
    } catch (error) {
      showResult(String(error));
    }
  };
}
