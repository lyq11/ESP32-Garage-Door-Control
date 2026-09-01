import { api, post } from "./api.js?v=20260829-io8-wake";
import { $, kv, val } from "./dom.js?v=20260829-io8-wake";
import { showResult } from "./logs.js?v=20260829-io8-wake";

let doorLimitConfigLoaded = false;

function updateDoorLimitControls() {
  const dual = val("doorLimitMode") === "DUAL";
  $("doorSinglePort").disabled = dual;
  $("doorTravelTimeoutSeconds").disabled = !dual;
  $("doorLimitHint").textContent = dual
    ? "上下门限固定：RS485-1 = 上门限（全开），RS485-2 = 下门限（全关）。两端都未触发时判断运动、中途停止或行程超时。"
    : `单门限使用 ${Number(val("doorSinglePort")) === 0 ? "RS485-1" : "RS485-2"} 作为关门到位信号；长时间未检测到该信号时沿用现有自动关门逻辑。`;
}

function fillDoorLimitConfig(config, force = false) {
  if (!config || (doorLimitConfigLoaded && !force)) return;
  $("doorLimitMode").value = config.mode || "SINGLE";
  $("doorSinglePort").value = String(config.singlePort ?? 1);
  $("doorTravelTimeoutSeconds").value = config.travelTimeoutSeconds ?? 60;
  doorLimitConfigLoaded = true;
  updateDoorLimitControls();
}

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
  const [limitConfig, rs4851, rs4852] = await Promise.all([
    api("/api/rs485/door-config"),
    api("/api/rs485/1/status"),
    api("/api/rs485/2/status")
  ]);
  fillDoorLimitConfig(limitConfig);
  $("doorLimitConfigStatus").innerHTML = kv(limitConfig);
  const dual = limitConfig.mode === "DUAL";
  const selectedPort = Number(limitConfig.singlePort ?? 1);
  $("rs485Status").innerHTML = `
    <h3>RS485-1（${dual ? "上门限 / 全开" : selectedPort === 0 ? "单门限 / 全关" : "已关闭"}）</h3>
    ${kv(rs4851)}
    <h3>RS485-2（${dual ? "下门限 / 全关" : selectedPort === 1 ? "单门限 / 全关" : "已关闭"}）</h3>
    ${kv(rs4852)}
  `;
}

export function bindSensors() {
  $("doorLimitMode").onchange = updateDoorLimitControls;
  $("doorSinglePort").onchange = updateDoorLimitControls;
  $("btnSaveDoorLimit").onclick = async () => {
    try {
      const result = await post("/api/rs485/door-config", {
        mode: val("doorLimitMode"),
        singlePort: val("doorSinglePort"),
        travelTimeoutSeconds: val("doorTravelTimeoutSeconds")
      });
      if (result.config) fillDoorLimitConfig(result.config, true);
      showResult(result);
      await refreshSensors();
    } catch (error) {
      showResult(String(error));
    }
  };

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
