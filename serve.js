#!/usr/bin/env node

const http = require("http");
const fs = require("fs");
const path = require("path");

const root = __dirname;
const port = Number(process.env.PORT || process.argv[2] || 8123);

const contentTypes = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".jpeg": "image/jpeg",
  ".jpg": "image/jpeg",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".mov": "video/quicktime",
  ".mp4": "video/mp4",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".webp": "image/webp",
  ".zip": "application/zip",
};

function sendError(res, status, message) {
  res.writeHead(status, { "Content-Type": "text/plain; charset=utf-8" });
  res.end(message);
}

function getFilePath(urlPath) {
  const decodedPath = decodeURIComponent(urlPath.split("?")[0]);
  const normalizedPath = path.normalize(decodedPath).replace(/^(\.\.[/\\])+/, "");
  const requestedPath = path.join(root, normalizedPath);

  if (!requestedPath.startsWith(root)) {
    return null;
  }

  return requestedPath;
}

function streamFile(req, res, filePath, stat) {
  const ext = path.extname(filePath).toLowerCase();
  const contentType = contentTypes[ext] || "application/octet-stream";
  const range = req.headers.range;

  res.setHeader("Accept-Ranges", "bytes");
  res.setHeader("Content-Type", contentType);

  if (!range) {
    res.writeHead(200, { "Content-Length": stat.size });
    if (req.method === "HEAD") {
      res.end();
      return;
    }
    fs.createReadStream(filePath).pipe(res);
    return;
  }

  const match = range.match(/^bytes=(\d*)-(\d*)$/);
  if (!match) {
    sendError(res, 416, "Invalid range");
    return;
  }

  let start = match[1] === "" ? 0 : Number(match[1]);
  let end = match[2] === "" ? stat.size - 1 : Number(match[2]);

  if (match[1] === "" && match[2] !== "") {
    const suffixLength = Number(match[2]);
    start = Math.max(stat.size - suffixLength, 0);
    end = stat.size - 1;
  }

  if (Number.isNaN(start) || Number.isNaN(end) || start > end || start >= stat.size) {
    res.writeHead(416, { "Content-Range": `bytes */${stat.size}` });
    res.end();
    return;
  }

  end = Math.min(end, stat.size - 1);
  const chunkSize = end - start + 1;

  res.writeHead(206, {
    "Content-Length": chunkSize,
    "Content-Range": `bytes ${start}-${end}/${stat.size}`,
  });

  if (req.method === "HEAD") {
    res.end();
    return;
  }

  fs.createReadStream(filePath, { start, end }).pipe(res);
}

const server = http.createServer((req, res) => {
  if (req.method !== "GET" && req.method !== "HEAD") {
    sendError(res, 405, "Method not allowed");
    return;
  }

  const filePath = getFilePath(req.url);
  if (!filePath) {
    sendError(res, 403, "Forbidden");
    return;
  }

  fs.stat(filePath, (error, stat) => {
    if (error) {
      sendError(res, 404, "Not found");
      return;
    }

    if (stat.isDirectory()) {
      const indexPath = path.join(filePath, "index.html");
      fs.stat(indexPath, (indexError, indexStat) => {
        if (indexError || !indexStat.isFile()) {
          sendError(res, 404, "Not found");
          return;
        }
        streamFile(req, res, indexPath, indexStat);
      });
      return;
    }

    streamFile(req, res, filePath, stat);
  });
});

server.listen(port, () => {
  console.log(`Serving ${root}`);
  console.log(`Open http://localhost:${port}/`);
  console.log("Video scrubbing is enabled with byte-range requests.");
});
