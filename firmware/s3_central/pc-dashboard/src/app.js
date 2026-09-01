import { saveBase } from "./api.js?v=20260831-ntp-time";
import { $ } from "./dom.js?v=20260831-heap-stats";
import { bindFace, refreshFace } from "./face.js?v=20260831-ntp-time";
import { bindGarage, refreshGarage, refreshGarageRecords } from "./garage.js?v=20260831-ntp-time";
import { bindLogs, showResult } from "./logs.js?v=20260831-ntp-time";
import { bindSensors, refreshSensors } from "./sensors.js?v=20260831-ntp-time";
import { bindSettings, refreshSettings } from "./settings.js?v=20260831-ntp-time";
import { refreshSystem } from "./system.js?v=20260831-wifi-info";

function switchTab(name) {
  document.querySelectorAll(".tab").forEach((tab) => {
    tab.classList.toggle("active", tab.dataset.tab === name);
  });
  document.querySelectorAll(".tab-panel").forEach((panel) => {
    panel.classList.toggle("active", panel.id === `tab-${name}`);
  });
}

async function refreshAll() {
  try {
    await refreshSystem();
    await refreshFace();
    await refreshGarage();
    await refreshGarageRecords();
    await refreshSensors();
    await refreshSettings();
  } catch (error) {
    showResult(String(error));
  }
}

function bindTabs() {
  document.querySelectorAll(".tab").forEach((tab) => {
    tab.onclick = () => switchTab(tab.dataset.tab);
  });
}

function bindTopbar() {
  $("base").value = localStorage.getItem("centrBase") || "";
  $("saveBase").onclick = () => {
    saveBase();
    refreshAll();
  };
  $("refreshAll").onclick = refreshAll;
}

function bindAll() {
  bindTabs();
  bindTopbar();
  bindFace();
  bindGarage();
  bindSensors();
  bindSettings();
  bindLogs();
}

bindAll();
refreshAll();
setInterval(refreshAll, 3000);
