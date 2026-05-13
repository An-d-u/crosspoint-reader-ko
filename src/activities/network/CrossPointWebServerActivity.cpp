#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 2;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;
constexpr uint16_t DNS_PORT = 53;

DNSServer* dnsServer = nullptr;
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Entering simple file transfer");
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  state = WebServerActivityState::AP_STARTING;
  requestUpdate();
  startAccessPoint();
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  state = WebServerActivityState::SHUTTING_DOWN;
  stopWebServer();

  MDNS.end();
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }

  WiFi.softAPdisconnect(true);
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
  LOG_DBG("WEBACT", "Simple file transfer stopped");
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting simple AP file transfer");

  WiFi.mode(WIFI_AP);
  delay(100);

  const bool apStarted = AP_PASSWORD && strlen(AP_PASSWORD) >= 8
                             ? WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS)
                             : WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  if (!apStarted) {
    LOG_ERR("WEBACT", "Failed to start AP");
    onGoHome();
    return;
  }

  delay(100);
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  MDNS.begin(AP_HOSTNAME);

  dnsServer = new DNSServer();
  if (dnsServer) {
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(DNS_PORT, "*", apIP);
  }

  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (!webServer->isRunning()) {
    LOG_ERR("WEBACT", "Failed to start simple upload server");
    webServer.reset();
    onGoHome();
    return;
  }

  state = WebServerActivityState::SERVER_RUNNING;
  requestUpdate();
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  if (state != WebServerActivityState::SERVER_RUNNING) {
    return;
  }

  if (dnsServer) {
    dnsServer->processNextRequest();
  }

  if (webServer && webServer->isRunning()) {
    esp_task_wdt_reset();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); ++i) {
      webServer->handleClient();
      if ((i & 0x0F) == 0x0F) {
        yield();
        mappedInput.update();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          onGoHome();
          return;
        }
      }
    }
    lastHandleClientTime = millis();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  if (state != WebServerActivityState::SERVER_RUNNING && state != WebServerActivityState::AP_STARTING) {
    return;
  }

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FILE_TRANSFER), nullptr);

  if (state == WebServerActivityState::AP_STARTING) {
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = (pageHeight - height) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
  } else {
    renderServerRunning();
  }

  renderer.displayBuffer();
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                    EpdFontFamily::BOLD);
  startY += height10 + metrics.verticalSpacing * 2;

  const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
  const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
  QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                    connectedSSID.c_str());

  startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                    EpdFontFamily::BOLD);
  startY += height10 + metrics.verticalSpacing * 2;

  const std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
  const std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";
  const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
  QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                    hostnameUrl.c_str());
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 100,
                    ipUrl.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
