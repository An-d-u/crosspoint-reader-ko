#pragma once

#include <HalStorage.h>
#include <WebServer.h>

#include <memory>

class CrossPointWebServer {
 public:
  CrossPointWebServer();
  ~CrossPointWebServer();

  void begin();
  void stop();
  void handleClient();

  bool isRunning() const { return running; }
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  FsFile uploadFile;
  String uploadPath;
  String uploadError;
  size_t uploadedBytes = 0;
  bool uploadSuccess = false;
  bool running = false;
  uint16_t port = 80;

  void handleRoot() const;
  void handleStatus() const;
  void handleTime();
  void handleUpload();
  void handleUploadDone();
  void handleNotFound() const;
};
