#pragma once

#include "http_server/HttpServer.h"

#ifndef CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH
#define CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH "/sdcard"
#endif

/**
 * @brief REST route modules. Each registers its endpoints on the server;
 * HttpService calls them in order on every server start.
 */
namespace Routes {

void registerWeb(Http::Server& server);     // /, /index.html, /setup
void registerWifi(Http::Server& server);    // /api/wifi/*, captive-portal probes
void registerFiles(Http::Server& server);   // /api/storage/info, /api/files*
void registerAudio(Http::Server& server);   // /api/audio/*
void registerMusic(Http::Server& server);   // /api/music/*
void registerSystem(Http::Server& server);  // /api/system/*, /api/logs, /api/led/set
void registerConfig(Http::Server& server);  // /api/config/*
void registerOta(Http::Server& server);     // /api/ota*

} // namespace Routes
