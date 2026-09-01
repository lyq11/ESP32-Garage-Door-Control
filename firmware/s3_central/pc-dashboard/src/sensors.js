import { api, post } from "./api.js?v=20260829-io8-wake";
import { $, kv, val } from "./dom.js?v=20260829-io8-wake";
import { showResult } from "./logs.js?v=20260829-io8-wake";

function sensorConfigPayload() {
  return {
    port: val("cfgPort"),
    addr: val("cfgAddr"),
    modbusAddr: val("cfgModbusAddr"),
    magHoldMs: val("cfgMagHoldMs"),
    magReleaseMs: val("cfgMagReleaseMs"),
    vibrationWindowMs: val("cfgVibrationWindowMs"),
    vibrationThreshold: val("cfgVibrationThreshold"),
    runHoldMs: val("cfgRunHoldMs"),
    ledEnabled: val("cfgLedEnabled")
  };
}

function fillSensorConfig(data) {
  if (!data || !data.config) return;
  const cfg = data.config;
  $("cfgModbusAddr").value = cfg.modbusAddr ?? $("cfgModbusAddr").value;
  $("cfgAddr").value = cfg.modbusAddr ?? $("cfgAddr").value;
  $("cfgMagHoldMs").value = cfg.magHoldMs ?? $("cfgMagHoldMs").value;
  $("cfgMagReleaseMs").value = cfg.magReleaseMs ?? $("cfgMagReleaseMs").value;
  $("cfgVibrationWindowMs").value = cfg.vibrationWindowMs ?? $("cfgVibrationWindowMs").value;
  $("cfgVibrationThreshold").value = cfg.vibrationThreshold ?? $("cfgVibrationThreshold").value;
  $("cfgRunHoldMs").value = cfg.runHoldMs ?? $("cfgRunHoldMs").value;
  $("cfgLedEnabled").value = cfg.ledEnabled ? "true" : "false";
}

async function readSensorConfig() {
  const port = Number(val("cfgPort")) + 1;
  const data = await api(`/api/rs485/${port}/config`);
  fillSensorConfig(data);
  $("sensorConfigResult").textContent = JSON.stringify(data, null, 2);
  showResult(data);
}

export async function refreshSensors() {
  const [rs4851, rs4852] = await Promise.all([
    api("/api/rs485/1/status"),
    api("/api/rs485/2/status")
  ]);
  $("rs485Status").innerHTML = `
    <h3>RS485-1（仅采样，不唤醒人脸）</h3>
    ${kv(rs4851)}
    <h3>RS485-2</h3>
    ${kv(rs4852)}
  `;
}

export function bindSensors() {
  $("btnReadSensorConfig").onclick = async () => {
    try {
      await readSensorConfig();
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnSaveSensorConfig").onclick = async () => {
    try {
      const result = await post("/api/rs485/config", sensorConfigPayload());
      showResult(result);
      setTimeout(async () => {
        await readSensorConfig();
        await refreshSensors();
      }, 500);
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnReadRs485").onclick = async () => {
    try {
      const result = await post("/api/rs485/read", {
        port: val("readPort"),
        function: val("readFunction"),
        startReg: val("readStartReg"),
        count: val("readCount")
      });
      showResult(result);
    } catch (error) {
      showResult(String(error));
    }
  };

  $("btnWriteRs485").onclick = async () => {
    try {
      const result = await post("/api/rs485/write", {
        port: val("rsPort"),
        addr: val("rsAddr"),
        reg: val("rsReg"),
        value: val("rsValue")
      });
      showResult(result);
      setTimeout(refreshSensors, 500);
    } catch (error) {
      showResult(String(error));
    }
  };
}
