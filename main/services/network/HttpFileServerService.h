#pragma once

#include "common/ReactorTask.h"
#include "esp_http_server.h"
#include <string>

namespace Services {

class HttpFileServerService : public ReactorTask {
public:
    static HttpFileServerService& getInstance();

    HttpFileServerService();
    ~HttpFileServerService() override;

    bool begin();

    // ReactorTask interface: react to Wi-Fi connectivity in EmbeddedSysDb
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

    bool startServer();
    void stopServer();
    bool isRunning() const { return m_server != nullptr; }

protected:
    void run() override;

private:
    HttpFileServerService(const HttpFileServerService&) = delete;
    HttpFileServerService& operator=(const HttpFileServerService&) = delete;

    httpd_handle_t m_server = nullptr;
    bool m_wifi_was_connected = false;

    void registerUriHandlers();

    // Static URI Handlers — Files
    static esp_err_t indexHandler(httpd_req_t* req);
    static esp_err_t storageInfoHandler(httpd_req_t* req);
    static esp_err_t listFilesHandler(httpd_req_t* req);
    static esp_err_t downloadFileHandler(httpd_req_t* req);
    static esp_err_t uploadFileHandler(httpd_req_t* req);
    static esp_err_t mkdirHandler(httpd_req_t* req);
    static esp_err_t renameHandler(httpd_req_t* req);
    static esp_err_t deleteHandler(httpd_req_t* req);
    static esp_err_t optionsHandler(httpd_req_t* req);

    // Static URI Handlers — Audio & Voice
    static esp_err_t audioVolumeHandler(httpd_req_t* req);
    static esp_err_t audioMicGainHandler(httpd_req_t* req);
    static esp_err_t audioMicMuteHandler(httpd_req_t* req);
    static esp_err_t audioAlertHandler(httpd_req_t* req);
    static esp_err_t audioRecordHandler(httpd_req_t* req);

    // Static URI Handlers — Music & Library
    static esp_err_t musicPlayHandler(httpd_req_t* req);
    static esp_err_t musicPlayLocalHandler(httpd_req_t* req);
    static esp_err_t musicControlHandler(httpd_req_t* req);
    static esp_err_t musicStatusHandler(httpd_req_t* req);
    static esp_err_t musicLibraryHandler(httpd_req_t* req);
    static esp_err_t musicLibraryScanHandler(httpd_req_t* req);
    static esp_err_t musicLibraryDeleteHandler(httpd_req_t* req);

    // Static URI Handlers — LED Control
    static esp_err_t ledSetHandler(httpd_req_t* req);

    // Static URI Handlers — System Metrics & Telemetry
    static esp_err_t metricsHandler(httpd_req_t* req);
    static esp_err_t systemInitHandler(httpd_req_t* req);
    static esp_err_t systemDeltaHandler(httpd_req_t* req);

    // Static URI Handlers — Config Management
    static esp_err_t configGetHandler(httpd_req_t* req);
    static esp_err_t configSetHandler(httpd_req_t* req);

    // Static URI Handlers — Live Log Console
    static esp_err_t logsHandler(httpd_req_t* req);

    // Static URI Handlers — OTA & System
    static esp_err_t otaStatusHandler(httpd_req_t* req);
    static esp_err_t otaUploadHandler(httpd_req_t* req);
    static esp_err_t systemRebootHandler(httpd_req_t* req);

    // Helpers
    static bool getQueryParam(httpd_req_t* req, const char* param_name, std::string& out_val);
    static void urlDecode(const std::string& in, std::string& out);
    static esp_err_t sendJsonResponse(httpd_req_t* req, int status_code, const std::string& json_str);
    static esp_err_t sendJsonError(httpd_req_t* req, int status_code, const char* message);
    static const char* getMimeType(const std::string& path);

    static constexpr const char* TAG = "HttpFileServer";
};

} // namespace Services
