import { $, formBody } from "./dom.js?v=20260626-rs485-config";

export function base() {
  return $("base").value.trim().replace(/\/$/, "");
}

export function saveBase() {
  localStorage.setItem("centrBase", base());
}

export async function api(path, options = {}) {
  const response = await fetch(base() + path, options);
  if (!response.ok) {
    throw new Error(`请求失败：${response.status} ${response.statusText}`);
  }
  return response.json();
}

export async function post(path, obj) {
  return api(path, {
    method: "POST",
    body: formBody(obj)
  });
}
