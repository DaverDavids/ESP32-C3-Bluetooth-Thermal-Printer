#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Secrets.h>
#include "esp_ota_ops.h"

const char* hostname = "c3printer";

WebServer server(80);

void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    const esp_partition_t* ota0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    Serial.println("Factory: OTA start, writing to ota_0");
    Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH, -1, LOW, ota0);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Factory Update Mode");
  WiFi.mode(WIFI_STA);
  WiFi.begin(MYSSID, MYPSK);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  MDNS.begin(hostname);
  MDNS.addService("http", "tcp", 80);

  server.on("/ota_upload", HTTP_POST,
    [](){
      const esp_partition_t* ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (!Update.hasError() && ota0) esp_ota_set_boot_partition(ota0);
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK, rebooting into main app");
      delay(300);
      ESP.restart();
    },
    handleFirmwareUpload
  );

  server.on("/", HTTP_GET, [](){
    server.send(200, "text/html",
      "<h2>Firmware Recovery Mode</h2>"
      "<form method='POST' action='/ota_upload' enctype='multipart/form-data'>"
      "<input type='file' name='firmware'><input type='submit' value='Upload'></form>");
  });

  server.begin();
  Serial.println("Recovery mode ready at http://c3printer.local/");
}

void loop() {
  server.handleClient();
}
