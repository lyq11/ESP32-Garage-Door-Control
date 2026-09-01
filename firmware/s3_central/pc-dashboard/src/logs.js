import { api } from "./api.js?v=20260626-rs485-config";
import { $, zhLabel, zhValue } from "./dom.js?v=20260626-rs485-config";

function formatObject(value) {
  if (!value || typeof value !== "object") {
    return zhValue(value);
  }
  if (Array.isArray(value)) {
    return value.map(formatObject).join("\n");
  }
  return Object.keys(value)
    .map((key) => `${zhLabel(key)}: ${zhValue(value[key])}`)
    .join("\n");
}

export function showResult(value) {
  $("log").textContent = typeof value === "string" ? value : formatObject(value);
}

export async function refreshLogs() {
  const data = await api("/api/logs");
  $("log").textContent = data.logs
    .map((entry) => `[${entry.time}][${entry.level}][${entry.module}] ${entry.message}`)
    .join("\n");
}

export function bindLogs() {
  $("btnRefreshLogs").onclick = async () => {
    try {
      await refreshLogs();
    } catch (error) {
      showResult(String(error));
    }
  };
}
