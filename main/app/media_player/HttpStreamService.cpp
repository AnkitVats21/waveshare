#include "HttpStreamService.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/mqtt/MqttService.h"
#include "common/AppLogger.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/BufferManager.h"
#include <cstring>

static void saveLastQuery(const char *query) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err == ESP_OK) {
    nvs_set_str(my_handle, "last_query", query);
    nvs_commit(my_handle);
    nvs_close(my_handle);
  }
}

static void loadLastQuery(char *dest, size_t dest_size) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
  if (err == ESP_OK) {
    nvs_get_str(my_handle, "last_query", dest, &dest_size);
    nvs_close(my_handle);
  }
}

static void constructChunkUrl(const char *manifest_url, const char *token,
                              uint32_t index, char *out_url,
                              size_t out_url_size) {
  const char *path_pos = strstr(manifest_url, "/stream/manifest");
  if (path_pos) {
    size_t base_len = path_pos - manifest_url;
    if (base_len >= out_url_size)
      base_len = out_url_size - 1;
    strncpy(out_url, manifest_url, base_len);
    out_url[base_len] = '\0';
    snprintf(out_url + base_len, out_url_size - base_len,
             "/stream/chunk?token=%s&index=%lu", token, (unsigned long)index);
  } else {
    strncpy(out_url, manifest_url, out_url_size - 1);
    out_url[out_url_size - 1] = '\0';
  }
}

HttpStreamService &HttpStreamService::getInstance() {
  static HttpStreamService instance;
  return instance;
}

HttpStreamService::HttpStreamService()
    : TaskBase({
          "chunk_fetch_task", 4096,
          ThreadConfig::Priority::MQTT, // Core 0, priority 3 (matches MQTT in
                                        // thread_config)
          0                             // Core 0
      }) {
  m_event_group = xEventGroupCreate();
}

HttpStreamService::~HttpStreamService() {
  if (m_event_group) {
    vEventGroupDelete(m_event_group);
  }
}

bool HttpStreamService::begin() {
  ESP_LOGI(TAG, "HttpStreamService operational.");
  return true;
}

void HttpStreamService::startStream(const cJSON *payload) {
  cJSON *manifest_url_item = cJSON_GetObjectItem(payload, "manifest_url");
  cJSON *video_id_item = cJSON_GetObjectItem(payload, "video_id");
  cJSON *title_item = cJSON_GetObjectItem(payload, "title");
  cJSON *query_item = cJSON_GetObjectItem(payload, "query");

  const char *manifest_url =
      manifest_url_item ? manifest_url_item->valuestring : nullptr;
  const char *video_id = video_id_item ? video_id_item->valuestring : nullptr;
  const char *title = title_item ? title_item->valuestring : nullptr;
  const char *query = query_item ? query_item->valuestring : nullptr;

  if (!manifest_url) {
    ESP_LOGE(TAG, "startStream called without manifest_url");
    return;
  }

  if (query && strlen(query) > 0) {
    saveLastQuery(query);
  }

  if (m_state == PLAYER_FETCHING) {
    strncpy(m_pending.manifest_url, manifest_url,
            sizeof(m_pending.manifest_url) - 1);
    strncpy(m_pending.video_id, video_id ? video_id : "",
            sizeof(m_pending.video_id) - 1);
    strncpy(m_pending.title, title ? title : "", sizeof(m_pending.title) - 1);
    m_pending.valid = true;
    ESP_LOGI(TAG, "Pending track enqueued: %s", manifest_url);
  } else {
    strncpy(m_manifest.manifest_url, manifest_url,
            sizeof(m_manifest.manifest_url) - 1);
    strncpy(m_manifest.video_id, video_id ? video_id : "",
            sizeof(m_manifest.video_id) - 1);
    strncpy(m_manifest.title, title ? title : "", sizeof(m_manifest.title) - 1);
    m_manifest.valid = true;

    m_state = PLAYER_FETCHING;
    xEventGroupClearBits(m_event_group, PLAYER_PAUSE_BIT | PLAYER_RESUME_BIT |
                                            PLAYER_STOP_BIT);
    xEventGroupSetBits(m_event_group, STREAM_START_BIT);

    auto speaker = AudioPipelineManager::getSpeakerTask();
    if (speaker) {
      speaker->setActiveSource(AUDIO_SOURCE_HTTP);
    }
    ESP_LOGI(TAG, "Starting stream immediately: %s", manifest_url);
  }
}

void HttpStreamService::pauseStream() {
  ESP_LOGI(TAG, "Pause stream requested");
  xEventGroupSetBits(m_event_group, PLAYER_PAUSE_BIT);
  xEventGroupClearBits(m_event_group, PLAYER_RESUME_BIT);
  BufferManager::getInstance().flush(Buffers::CHUNK_PCM_BUF);
}

void HttpStreamService::resumeStream() {
  ESP_LOGI(TAG, "Resume stream requested");
  xEventGroupSetBits(m_event_group, PLAYER_RESUME_BIT);
  xEventGroupClearBits(m_event_group, PLAYER_PAUSE_BIT);

  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"cmd\":\"resume\",\"device_id\":\"%s\"}",
           CONFIG_WAVESHARE_MQTT_CLIENT_ID);
  MqttService::getInstance().publish("mpv/command", payload);
}

void HttpStreamService::stopStream() {
  ESP_LOGI(TAG, "Stop stream requested");
  xEventGroupSetBits(m_event_group, PLAYER_STOP_BIT);
  xEventGroupWaitBits(m_event_group, PLAYER_STOPPED_ACK_BIT, pdTRUE, pdTRUE,
                      pdMS_TO_TICKS(500));
  BufferManager::getInstance().flush(Buffers::CHUNK_PCM_BUF);
  m_state = PLAYER_IDLE;
}

void HttpStreamService::handleServerOnline() {
  char last_query[128] = {0};
  loadLastQuery(last_query, sizeof(last_query));
  if (strlen(last_query) > 0) {
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"cmd\":\"play\",\"query\":\"%s\",\"device_id\":\"%s\"}",
             last_query, CONFIG_WAVESHARE_MQTT_CLIENT_ID);
    MqttService::getInstance().publish("mpv/command", payload);
    ESP_LOGI(TAG, "Server online. Re-playing last query from NVS: %s",
             last_query);
  }
}

void HttpStreamService::suspendTask() {
  if (m_task_handle) {
    vTaskSuspend(m_task_handle);
    ESP_LOGI(TAG, "ChunkFetchTask suspended");
  }
}

void HttpStreamService::resumeTask() {
  if (m_task_handle) {
    vTaskResume(m_task_handle);
    ESP_LOGI(TAG, "ChunkFetchTask resumed");
  }
}

void HttpStreamService::applyPendingStream() {
  strncpy(m_manifest.manifest_url, m_pending.manifest_url,
          sizeof(m_manifest.manifest_url) - 1);
  strncpy(m_manifest.video_id, m_pending.video_id,
          sizeof(m_manifest.video_id) - 1);
  strncpy(m_manifest.title, m_pending.title, sizeof(m_manifest.title) - 1);
  m_manifest.valid = true;
  m_pending.valid = false;

  xEventGroupClearBits(m_event_group,
                       PLAYER_PAUSE_BIT | PLAYER_RESUME_BIT | PLAYER_STOP_BIT);
  xEventGroupSetBits(m_event_group, STREAM_START_BIT);
  m_state = PLAYER_FETCHING;
  ESP_LOGI(TAG, "Pending stream applied: %s", m_manifest.manifest_url);
}

void HttpStreamService::waitForBufferDrain(uint32_t timeout_ms) {
  auto &bm = BufferManager::getInstance();
  uint32_t elapsed = 0;
  while (bm.getUsedBytes(Buffers::CHUNK_PCM_BUF) > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
    elapsed += 50;
    if (elapsed >= timeout_ms) {
      ESP_LOGW(TAG, "Buffer drain timed out. Flushing buffer.");
      bm.flush(Buffers::CHUNK_PCM_BUF);
      break;
    }
  }
}

void HttpStreamService::publishPauseCommand(uint32_t last_chunk) {
  char payload[128];
  snprintf(payload, sizeof(payload), "{\"cmd\":\"pause\",\"last_chunk\":%lu}",
           (unsigned long)last_chunk);
  MqttService::getInstance().publish("mpv/command", payload);
  ESP_LOGI(TAG, "Published pause at chunk %lu", (unsigned long)last_chunk);
}

void HttpStreamService::publishReplayCommand(const char *video_id) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"cmd\":\"play\",\"query\":\"video_id:%s\"}", video_id);
  MqttService::getInstance().publish("mpv/command", payload);
  ESP_LOGI(TAG, "Published replay command for video %s", video_id);
}

bool HttpStreamService::httpGetManifest(const char *url,
                                        chunk_manifest_t *out_manifest) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.timeout_ms = 5000;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return false;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return false;
  }

  int content_length = esp_http_client_fetch_headers(client);
  if (content_length <= 0) {
    content_length = 2048;
  }

  char *buffer = (char *)malloc(content_length + 1);
  if (!buffer) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  int total_read = 0;
  int read_bytes = 0;
  while (total_read < content_length) {
    read_bytes = esp_http_client_read(client, buffer + total_read,
                                      content_length - total_read);
    if (read_bytes <= 0) {
      break;
    }
    total_read += read_bytes;
  }
  buffer[total_read] = '\0';

  bool parsed = false;
  cJSON *root = cJSON_Parse(buffer);
  if (root) {
    cJSON *token_item = cJSON_GetObjectItem(root, "token");
    cJSON *from_chunk_item = cJSON_GetObjectItem(root, "from_chunk");
    cJSON *total_chunks_item = cJSON_GetObjectItem(root, "total_chunks");

    if (token_item && cJSON_IsString(token_item)) {
      strcpy(out_manifest->token, token_item->valuestring);
      out_manifest->from_chunk =
          (from_chunk_item && cJSON_IsNumber(from_chunk_item))
              ? from_chunk_item->valueint
              : 0;
      out_manifest->total_chunks =
          (total_chunks_item && cJSON_IsNumber(total_chunks_item))
              ? total_chunks_item->valueint
              : -1;
      parsed = true;
    }
    cJSON_Delete(root);
  }

  free(buffer);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return parsed;
}

esp_http_client_handle_t HttpStreamService::httpClientInit(const char *token) {
  char url[256];
  constructChunkUrl(m_manifest.manifest_url, token, 0, url, sizeof(url));

  esp_http_client_config_t config = {};
  config.url = url;
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;
  config.timeout_ms = 5000;

  return esp_http_client_init(&config);
}

HttpStreamService::http_chunk_result_t
HttpStreamService::httpGetChunk(esp_http_client_handle_t client,
                                const char *token, uint32_t index, uint8_t *buf,
                                size_t buf_size) {

  char url[256];
  constructChunkUrl(m_manifest.manifest_url, token, index, url, sizeof(url));

  esp_http_client_set_url(client, url);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    return {.status = -1, .bytes_read = 0, .is_last_chunk = false};
  }

  int status = esp_http_client_fetch_headers(client);
  if (status < 0) {
    esp_http_client_close(client);
    return {.status = -1, .bytes_read = 0, .is_last_chunk = false};
  }

  int http_status = esp_http_client_get_status_code(client);
  size_t total = 0;

  if (http_status == 200) {
    int n;
    while (total < buf_size) {
      n = esp_http_client_read(client, (char *)buf + total, buf_size - total);
      if (n <= 0) {
        break;
      }
      total += n;
    }
  }

  char *val_ptr = nullptr;
  esp_http_client_get_header(client, "X-Last-Chunk", &val_ptr);
  bool is_last = (val_ptr && strcmp(val_ptr, "true") == 0);

  esp_http_client_close(client);

  return {.status = http_status, .bytes_read = total, .is_last_chunk = is_last};
}

void HttpStreamService::run() {
  uint8_t *chunk_buf =
      (uint8_t *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!chunk_buf) {
    ESP_LOGE(TAG, "Failed to allocate 32KB chunk buffer in PSRAM!");
    return;
  }
  auto &bm = BufferManager::getInstance();
  EventGroupHandle_t evGroup = m_event_group;

  ESP_LOGI(TAG, "ChunkFetchTask active on Core %d", xPortGetCoreID());

  for (;;) {
    ESP_LOGI(TAG, "Waiting for stream start signal...");
    xEventGroupWaitBits(evGroup, STREAM_START_BIT, pdTRUE, pdTRUE,
                        portMAX_DELAY);

    ESP_LOGI(TAG, "Fetching manifest from URL: %s", m_manifest.manifest_url);

    chunk_manifest_t local_manifest = {};
    if (!httpGetManifest(m_manifest.manifest_url, &local_manifest)) {
      ESP_LOGE(TAG, "Failed to fetch manifest");
      m_state = PLAYER_IDLE;
      continue;
    }

    strcpy(m_manifest.token, local_manifest.token);
    m_manifest.from_chunk = local_manifest.from_chunk;
    m_manifest.total_chunks = local_manifest.total_chunks;
    m_next_chunk = m_manifest.from_chunk;
    m_state = PLAYER_FETCHING;

    ESP_LOGI(TAG, "Manifest loaded. token=%s, from_chunk=%lu, total_chunks=%d",
             m_manifest.token, (unsigned long)m_next_chunk,
             m_manifest.total_chunks);

    esp_http_client_handle_t client = httpClientInit(m_manifest.token);
    if (!client) {
      ESP_LOGE(TAG, "Failed to initialize HTTP client");
      m_state = PLAYER_IDLE;
      continue;
    }

    while (m_state == PLAYER_FETCHING) {
      EventBits_t bits = xEventGroupGetBits(evGroup);
      if (bits & PLAYER_STOP_BIT) {
        ESP_LOGI(TAG, "Stop bit set. Exiting fetch loop.");
        xEventGroupClearBits(evGroup, PLAYER_STOP_BIT);
        xEventGroupSetBits(evGroup, PLAYER_STOPPED_ACK_BIT);
        m_state = PLAYER_IDLE;
        break;
      }
      if (bits & PLAYER_PAUSE_BIT) {
        ESP_LOGI(TAG, "Pause bit set. Entering paused state.");
        uint32_t paused_at = m_next_chunk > 0 ? m_next_chunk - 1 : 0;
        publishPauseCommand(paused_at);
        m_state = PLAYER_PAUSED;

        ESP_LOGI(TAG, "Waiting for resume or stop signal...");
        EventBits_t wait_bits =
            xEventGroupWaitBits(evGroup, PLAYER_RESUME_BIT | PLAYER_STOP_BIT,
                                pdTRUE, pdFALSE, portMAX_DELAY);
        if (wait_bits & PLAYER_STOP_BIT) {
          ESP_LOGI(TAG, "Stopped while paused.");
          xEventGroupClearBits(evGroup, PLAYER_STOP_BIT);
          xEventGroupSetBits(evGroup, PLAYER_STOPPED_ACK_BIT);
          m_state = PLAYER_IDLE;
        } else {
          ESP_LOGI(TAG, "Resumed stream.");
          m_state = PLAYER_IDLE;
        }
        break;
      }

      if (m_pending.valid && bm.getUsedBytes(Buffers::CHUNK_PCM_BUF) == 0) {
        ESP_LOGI(
            TAG,
            "Pending track is valid and buffer is empty. Switching track.");
        applyPendingStream();
        break;
      }

      http_chunk_result_t res = httpGetChunk(
          client, m_manifest.token, m_next_chunk, chunk_buf, sizeof(chunk_buf));

      if (res.status == 200) {
        bm.send(Buffers::CHUNK_PCM_BUF, chunk_buf, res.bytes_read,
                portMAX_DELAY);
        m_next_chunk++;

        if (res.is_last_chunk) {
          ESP_LOGI(TAG, "Last chunk fetched. Waiting for buffer drain.");
          waitForBufferDrain(5000);
          if (m_pending.valid) {
            ESP_LOGI(TAG,
                     "Applying pending stream after natural end of track.");
            applyPendingStream();
            break;
          }
          m_state = PLAYER_IDLE;
          break;
        }
      } else if (res.status == 410) {
        ESP_LOGW(
            TAG,
            "Chunk %lu is evicted (410 Gone). Re-syncing via replay command.",
            (unsigned long)m_next_chunk);
        publishReplayCommand(m_manifest.video_id);
        m_state = PLAYER_IDLE;
        break;
      } else if (res.status == 403 || res.status == 204) {
        ESP_LOGW(
            TAG,
            "Token expired (403) or stream finished (204). Ending session.");
        m_state = PLAYER_IDLE;
        break;
      } else {
        ESP_LOGW(TAG, "Chunk fetch error %d. Retrying in 500ms...", res.status);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }

    esp_http_client_cleanup(client);
  }
  heap_caps_free(chunk_buf);
}
