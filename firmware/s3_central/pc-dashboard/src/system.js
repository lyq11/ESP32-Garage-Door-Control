import { api } from "./api.js?v=20260831-wifi-info";
import { $, kv } from "./dom.js?v=20260831-wifi-info";

let lastStatus = null;

export async function refreshSystem() {
  lastStatus = await api("/api/status");
  $("systemStatus").innerHTML = kv(lastStatus);
  const ip = lastStatus.ip || "device-ip";
  $("otaCommand").textContent =
    `C:\\Tools\\arduino-cli\\arduino-cli.exe upload -p ${ip} --fqbn esp32:esp32:esp32s3 --input-dir .arduino-build`;
}

export function getLastStatus() {
  return lastStatus;
}
