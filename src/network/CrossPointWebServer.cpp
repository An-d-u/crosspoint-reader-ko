#include "CrossPointWebServer.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

namespace {
constexpr const char* BOOKS_DIR = "/books";

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>CrossPoint File Transfer</title>
  <style>
    body{font-family:sans-serif;margin:24px;line-height:1.45;background:#f7f7f2;color:#111}
    main{max-width:560px;margin:auto;background:#fff;padding:20px;border:1px solid #ddd;border-radius:12px}
    h1{font-size:22px;margin:0 0 12px}
    input,button{font-size:16px;width:100%;box-sizing:border-box;margin-top:12px}
    button{padding:12px;border:0;border-radius:8px;background:#111;color:#fff}
    button:disabled{background:#888}
    .hint{color:#555;font-size:14px}
    .drop-zone{margin-top:12px;padding:18px;border:2px dashed #aaa;border-radius:10px;text-align:center;cursor:pointer;transition:border-color .15s,background .15s}
    .drop-zone.dragover{border-color:#111;background:#f0f0eb}
    .drop-zone input{margin-top:0}
    .drop-hint{margin:8px 0 0;color:#555;font-size:14px;pointer-events:none}
    .file{margin-top:14px;padding:10px;border:1px solid #ddd;border-radius:8px;background:#fafafa}
    .name{font-weight:700;word-break:break-all}
    .meta{font-size:13px;color:#555;margin-top:4px}
    .bar{height:10px;background:#e1e1dc;border-radius:999px;overflow:hidden;margin-top:8px}
    .fill{height:100%;width:0;background:#111;transition:width .15s linear}
    .ok .fill{background:#247a36}
    .err .fill{background:#b42318}
  </style>
</head>
<body>
  <main>
    <h1>CrossPoint File Transfer</h1>
    <p class="hint">EPUB/TXT/XTC files are saved to <code>/books</code>.</p>
    <form id="uploadForm">
      <div id="dropZone" class="drop-zone" role="button" tabindex="0">
        <input id="fileInput" type="file" name="file" accept=".epub,.txt,.xtc,.xtch,application/epub+zip,text/plain" multiple required>
        <p class="drop-hint">Drop files here or click to select</p>
      </div>
      <button id="uploadButton" type="submit">Upload</button>
    </form>
    <section id="progressList"></section>
  </main>
  <script>
    const form = document.getElementById('uploadForm');
    const input = document.getElementById('fileInput');
    const button = document.getElementById('uploadButton');
    const list = document.getElementById('progressList');
    const dropZone = document.getElementById('dropZone');
    let dragDepth = 0;

    function openFilePicker(event) {
      if (!input.disabled && event.target !== input) input.click();
    }

    dropZone.addEventListener('click', openFilePicker);
    dropZone.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        openFilePicker(event);
      }
    });
    dropZone.addEventListener('dragenter', (event) => {
      event.preventDefault();
      dragDepth++;
      if (!input.disabled) dropZone.classList.add('dragover');
    });
    dropZone.addEventListener('dragover', (event) => {
      // 브라우저가 드롭한 파일을 직접 열지 않도록 기본 동작을 막는다.
      event.preventDefault();
    });
    dropZone.addEventListener('dragleave', (event) => {
      event.preventDefault();
      dragDepth = Math.max(0, dragDepth - 1);
      if (dragDepth === 0) dropZone.classList.remove('dragover');
    });
    dropZone.addEventListener('drop', (event) => {
      event.preventDefault();
      dragDepth = 0;
      dropZone.classList.remove('dragover');
      if (input.disabled || !event.dataTransfer || !event.dataTransfer.files.length) return;

      // 기존 선택 및 업로드 흐름을 그대로 재사용한다.
      const transfer = new DataTransfer();
      for (const file of event.dataTransfer.files) transfer.items.add(file);
      input.files = transfer.files;
    });

    function fmt(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / 1024 / 1024).toFixed(1) + ' MB';
    }

    function rowFor(file) {
      const row = document.createElement('div');
      row.className = 'file';
      row.innerHTML =
        '<div class="name"></div>' +
        '<div class="meta">Waiting · 0% · 0 B / ' + fmt(file.size) + '</div>' +
        '<div class="bar"><div class="fill"></div></div>';
      row.querySelector('.name').textContent = file.name;
      list.appendChild(row);
      return row;
    }

    function uploadOne(file, row) {
      return new Promise((resolve) => {
        const meta = row.querySelector('.meta');
        const fill = row.querySelector('.fill');
        const data = new FormData();
        data.append('file', file, file.name);

        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/upload');
        xhr.upload.onprogress = (event) => {
          if (!event.lengthComputable) {
            meta.textContent = 'Uploading...';
            return;
          }
          const pct = Math.round((event.loaded / event.total) * 100);
          fill.style.width = pct + '%';
          meta.textContent = 'Uploading · ' + pct + '% · ' + fmt(event.loaded) + ' / ' + fmt(event.total);
        };
        xhr.onload = () => {
          if (xhr.status >= 200 && xhr.status < 300) {
            row.classList.add('ok');
            fill.style.width = '100%';
            meta.textContent = 'Complete · 100% · ' + fmt(file.size);
          } else {
            row.classList.add('err');
            meta.textContent = 'Failed · ' + (xhr.responseText || xhr.status);
          }
          resolve();
        };
        xhr.onerror = () => {
          row.classList.add('err');
          meta.textContent = 'Failed · Network error';
          resolve();
        };
        xhr.send(data);
      });
    }

    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      const files = Array.from(input.files || []);
      if (!files.length) return;

      button.disabled = true;
      input.disabled = true;
      list.innerHTML = '';
      const rows = files.map(rowFor);

      for (let i = 0; i < files.length; i++) {
        await uploadOne(files[i], rows[i]);
      }

      button.disabled = false;
      input.disabled = false;
    });
  </script>
</body>
</html>
)HTML";

String sanitizeFilename(String name) {
  name.replace("\\", "/");
  const int slash = name.lastIndexOf('/');
  if (slash >= 0) {
    name = name.substring(slash + 1);
  }
  name.trim();
  name.replace("..", "_");
  name.replace("/", "_");
  if (name.isEmpty()) {
    name = "upload.bin";
  }
  return name;
}

void clearEpubCacheIfNeeded(const String& filePath) {
  if (FsHelpers::hasEpubExtension(filePath)) {
    Epub(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("WEB", "Cleared epub cache for: %s", filePath.c_str());
  }
}
}  // namespace

CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    return;
  }

  const wifi_mode_t wifiMode = WiFi.getMode();
  if (!(wifiMode & WIFI_MODE_AP)) {
    LOG_ERR("WEB", "AP mode is not active; cannot start upload server");
    return;
  }

  if (!Storage.exists(BOOKS_DIR) && !Storage.mkdir(BOOKS_DIR)) {
    LOG_ERR("WEB", "Failed to create %s", BOOKS_DIR);
    return;
  }

  WiFi.setSleep(false);
  server.reset(new WebServer(port));
  if (!server) {
    LOG_ERR("WEB", "Failed to allocate WebServer");
    return;
  }

  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/upload", HTTP_POST, [this] { handleUploadDone(); }, [this] { handleUpload(); });
  server->onNotFound([this] { handleNotFound(); });
  server->begin();

  running = true;
  LOG_DBG("WEB", "Simple upload server started at http://%s/", WiFi.softAPIP().toString().c_str());
}

void CrossPointWebServer::stop() {
  running = false;
  if (uploadFile) {
    uploadFile.close();
  }
  if (!uploadPath.isEmpty() && !uploadSuccess) {
    Storage.remove(uploadPath.c_str());
  }
  uploadPath = "";
  uploadError = "";
  uploadedBytes = 0;
  uploadSuccess = false;

  if (server) {
    server->stop();
    server.reset();
  }
}

void CrossPointWebServer::handleClient() {
  if (running && server) {
    server->handleClient();
  }
}

void CrossPointWebServer::handleRoot() const { server->send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

void CrossPointWebServer::handleStatus() const { server->send(200, "application/json", "{\"ok\":true}"); }

void CrossPointWebServer::handleUpload() {
  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadSuccess = false;
    uploadedBytes = 0;
    uploadError = "";
    const String safeName = sanitizeFilename(upload.filename);
    uploadPath = String(BOOKS_DIR) + "/" + safeName;

    if (Storage.exists(uploadPath.c_str())) {
      Storage.remove(uploadPath.c_str());
    }
    if (!Storage.openFileForWrite("WEB", uploadPath, uploadFile)) {
      uploadError = "Failed to open file";
      LOG_ERR("WEB", "%s: %s", uploadError.c_str(), uploadPath.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadError.isEmpty() || !uploadFile) {
      return;
    }
    const size_t written = uploadFile.write(upload.buf, upload.currentSize);
    uploadedBytes += written;
    if (written != upload.currentSize) {
      uploadError = "SD write failed";
      LOG_ERR("WEB", "Upload write failed: wrote=%u expected=%u", written, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
    }
    uploadSuccess = uploadError.isEmpty() && uploadedBytes > 0;
    if (uploadSuccess) {
      clearEpubCacheIfNeeded(uploadPath);
      LOG_DBG("WEB", "Upload complete: %s (%u bytes)", uploadPath.c_str(), uploadedBytes);
    } else if (!uploadPath.isEmpty()) {
      Storage.remove(uploadPath.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
    }
    if (!uploadPath.isEmpty()) {
      Storage.remove(uploadPath.c_str());
    }
    uploadError = "Upload aborted";
    uploadSuccess = false;
  }
}

void CrossPointWebServer::handleUploadDone() {
  if (uploadSuccess) {
    const String body = "<!doctype html><meta charset=\"utf-8\"><p>Upload complete: " + uploadPath +
                        "</p><p><a href=\"/\">Back</a></p>";
    server->send(200, "text/html; charset=utf-8", body);
  } else {
    const String error = uploadError.isEmpty() ? "Upload failed" : uploadError;
    server->send(500, "text/plain; charset=utf-8", error);
  }
}

void CrossPointWebServer::handleNotFound() const {
  server->sendHeader("Location", "/", true);
  server->send(302, "text/plain", "");
}
