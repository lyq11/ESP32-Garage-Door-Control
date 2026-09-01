import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL(".", import.meta.url));
const args = new Map(
  process.argv.slice(2).map((arg) => {
    const [key, ...rest] = arg.replace(/^--/, "").split("=");
    return [key, rest.join("=")];
  })
);

const port = Number(args.get("port") || process.env.npm_config_port || process.env.PORT || 5173);
const target = (
  args.get("target") ||
  process.env.npm_config_target ||
  process.env.DEVICE_URL ||
  "http://192.168.4.1"
).replace(/\/$/, "");

const contentTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8"
};

async function serveFile(res, filePath) {
  try {
    const data = await readFile(filePath);
    res.writeHead(200, {
      "Content-Type": contentTypes[extname(filePath)] || "application/octet-stream",
      "Cache-Control": "no-store"
    });
    res.end(data);
  } catch {
    res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
    res.end("not found");
  }
}

async function proxy(req, res) {
  const url = new URL(req.url, target);
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const body = chunks.length ? Buffer.concat(chunks) : undefined;

  try {
    const upstream = await fetch(url, {
      method: req.method,
      headers: {
        "content-type": req.headers["content-type"] || "application/x-www-form-urlencoded"
      },
      body
    });
    const data = Buffer.from(await upstream.arrayBuffer());
    res.writeHead(upstream.status, {
      "Content-Type": upstream.headers.get("content-type") || "application/json; charset=utf-8",
      "Access-Control-Allow-Origin": "*",
      "Cache-Control": "no-store"
    });
    res.end(data);
  } catch (error) {
    res.writeHead(502, { "Content-Type": "application/json; charset=utf-8" });
    res.end(JSON.stringify({ ok: false, error: "proxy_failed", target, detail: String(error) }));
  }
}

createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type"
    });
    res.end();
    return;
  }

  if (req.url.startsWith("/api/")) {
    await proxy(req, res);
    return;
  }

  if (req.url === "/" || req.url.startsWith("/index.html")) {
    await serveFile(res, join(root, "index.html"));
    return;
  }

  await serveFile(res, join(root, decodeURIComponent(new URL(req.url, "http://localhost").pathname)));
}).listen(port, () => {
  console.log(`PC dashboard: http://127.0.0.1:${port}`);
  console.log(`Proxy target: ${target}`);
});
