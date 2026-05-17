#include "app/wake_word/WakeWordDetector.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_process_sdkconfig.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "common/AppLogger.h"
#include "common/app_types.h"
#include "hal/Board.h"
#include "services/EventBus.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
// Singleton
// ============================================================================

WakeWordDetector &WakeWordDetector::getInstance() {
  static WakeWordDetector instance;
  return instance;
}

// ============================================================================
// Public API
// ============================================================================

void WakeWordDetector::registerCallback(wake_word_callback_t cb,
                                        void *user_data) {
  m_callback = cb;
  m_user_data = user_data;
}

bool WakeWordDetector::begin() {
  if (m_task_flag) {
    ESP_LOGW(TAG, "Already running");
    return true;
  }

  Board &board = Board::getInstance();
  if (!board.isInitialized()) {
    ESP_LOGE(TAG, "Board not initialized — call Board::begin() first");
    return false;
  }

  // 1. Load SR models from the 'model' flash partition
  srmodel_list_t *models = esp_srmodel_init("model");
  if (!models) {
    ESP_LOGE(
        TAG,
        "esp_srmodel_init failed — check partitions.csv for 'model' partition");
    return false;
  }

  // 2. Build AFE config (4-channel RMNM, low-cost mode)
  const char *input_format = board.getInputFormat(); // "RMNM"
  afe_config_t *afe_config =
      afe_config_init(input_format, models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  afe_config->ns_init = false;
  afe_config->vad_init = true; // needed for VAD-based streaming timeout

  // 3. Create AFE handle + data
  m_afe_handle = esp_afe_handle_from_config(afe_config);
  if (!m_afe_handle) {
    ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
    afe_config_free(afe_config);
    return false;
  }

  esp_afe_sr_data_t *afe_data = m_afe_handle->create_from_config(afe_config);
  afe_config_free(afe_config);
  if (!afe_data) {
    ESP_LOGE(TAG, "AFE create_from_config failed");
    return false;
  }

  m_task_flag = 1;

  // 4. Launch tasks  (detect on Core 1, feed on Core 0 — mirrors demo BSP)
  xTaskCreatePinnedToCore(detectTaskBridge, "ww_detect", 8 * 1024,
                          (void *)afe_data, 5, nullptr, 1);
  xTaskCreatePinnedToCore(feedTaskBridge, "ww_feed", 8 * 1024, (void *)afe_data,
                          5, nullptr, 0);

  LOGI_SYSTEM("WakeWordDetector started");
  return true;
}

void WakeWordDetector::stop() {
  m_task_flag = 0;
  // Tasks will exit on next loop iteration; AFE data freed by the detect task
  ESP_LOGI(TAG, "WakeWordDetector stop requested");
}

// ============================================================================
// FreeRTOS task bridges
// ============================================================================

void WakeWordDetector::feedTaskBridge(void *arg) {
  // afe_data was passed as arg; but we need the singleton for m_task_flag
  // and m_afe_handle.  Route through singleton.
  WakeWordDetector::getInstance().feedTask(
      static_cast<esp_afe_sr_data_t *>(arg));
}

void WakeWordDetector::detectTaskBridge(void *arg) {
  WakeWordDetector::getInstance().detectTask(
      static_cast<esp_afe_sr_data_t *>(arg));
}

// ============================================================================
// Feed task: read 4-ch mic data from AudioHal → push to AFE
// ============================================================================

void WakeWordDetector::feedTask(esp_afe_sr_data_t *afe_data) {
  Board &board = Board::getInstance();

  int audio_chunksize = m_afe_handle->get_feed_chunksize(afe_data);
  int nch = m_afe_handle->get_feed_channel_num(afe_data);
  int feed_channel = board.getFeedChannel(); // 4

  if (nch != feed_channel) {
    ESP_LOGE(TAG, "AFE channel mismatch: AFE wants %d, board provides %d", nch,
             feed_channel);
    vTaskDelete(nullptr);
    return;
  }

  // 4-ch raw buffer for hardware read (SPIRAM)
  int buf_bytes = audio_chunksize * sizeof(int16_t) * feed_channel;
  int16_t *i2s_buff = (int16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
  if (!i2s_buff) {
    ESP_LOGE(TAG, "Failed to allocate feed buffer (%d bytes)", buf_bytes);
    vTaskDelete(nullptr);
    return;
  }

  // Mono Mic1 extraction buffer for the streaming ring buffer (IRAM-safe)
  // RMNM layout: slot0=Ref, slot1=Mic1, slot2=Noise, slot3=Mic2
  int mic_bytes = audio_chunksize * sizeof(int16_t);
  int16_t *mic1_buff = (int16_t *)heap_caps_malloc(
      mic_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!mic1_buff) {
    ESP_LOGE(TAG, "Failed to allocate mic1 buffer");
    heap_caps_free(i2s_buff);
    vTaskDelete(nullptr);
    return;
  }

  esp_task_wdt_add(nullptr);

  while (m_task_flag) {
    // ── Sole hardware read ──────────────────────────────────────────────────
    // Pause if AudioHal is being reinit'd (m_hw_valid cleared by AudioService)
    if (!m_hw_valid) {
      vTaskDelay(pdMS_TO_TICKS(10));
      esp_task_wdt_reset();
      continue;
    }

    board.getFeedData(/*raw=*/true, i2s_buff, buf_bytes);

    // Feed the AFE engine (wake-word + beamforming)
    m_afe_handle->feed(afe_data, i2s_buff);

    // ── Streaming path: extract Mic1 (slot 1) → tx ring buffer ─────────────
    // Only streams when wake word has been detected (m_streaming_active).
    RingbufHandle_t rb = m_stream_ringbuf;
    if (rb && m_streaming_active) {
      for (int i = 0; i < audio_chunksize; i++) {
        mic1_buff[i] = i2s_buff[i * feed_channel + 1]; // slot 1 = Mic1
      }
      xRingbufferSend(rb, mic1_buff, mic_bytes, 0);
    }

    esp_task_wdt_reset();
  }

  heap_caps_free(mic1_buff);
  heap_caps_free(i2s_buff);
  vTaskDelete(nullptr);
}

// ============================================================================
// Detect task: fetch AFE results → WakeNet → MultiNet → emit events
// ============================================================================

void WakeWordDetector::detectTask(esp_afe_sr_data_t *afe_data) {
  // Load the MultiNet (command recognition) model
  srmodel_list_t *models = esp_srmodel_init("model");
  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  if (!mn_name) {
    ESP_LOGW(TAG,
             "No MultiNet English model found; command recognition disabled");
  }

  esp_mn_iface_t *multinet =
      mn_name ? esp_mn_handle_from_name(mn_name) : nullptr;
  model_iface_data_t *model_data =
      (multinet && mn_name) ? multinet->create(mn_name, 8000) : nullptr;

  if (multinet && model_data) {
    esp_mn_commands_update_from_sdkconfig(multinet, model_data);
    multinet->print_active_speech_commands(model_data);
  }

  int wakeup_flag = 0;
  int silence_frames = 0; // consecutive VAD-silence AFE frames
  int fetch_chunksize = m_afe_handle->get_fetch_chunksize(afe_data);
  // Frames per second = sample_rate / fetch_chunksize
  // Silence timeout in frames (compute once):
  const int SILENCE_TIMEOUT_FRAMES =
      (VAD_SILENCE_TIMEOUT_MS * 16000) / (fetch_chunksize * 1000);

  esp_task_wdt_add(nullptr);

  while (m_task_flag) {
    afe_fetch_result_t *res = m_afe_handle->fetch(afe_data);
    if (!res || res->ret_value == ESP_FAIL) {
      ESP_LOGE(TAG, "AFE fetch error");
      break;
    }

    // ── Wake word detection ─────────────────────────────────────────────────
    if (res->wakeup_state == WAKENET_DETECTED) {
      if (multinet && model_data)
        multinet->clean(model_data);
    }

    if (res->raw_data_channels == 1 && res->wakeup_state == WAKENET_DETECTED) {
      wakeup_flag = 1;
      m_streaming_active = true;
      silence_frames = 0;
      LOGI_SYSTEM("Wake word detected (1-ch) — streaming started");
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::WAKE_WORD_DETECTED,
                                      (uint32_t)res->trigger_channel_id);
      if (m_callback) {
        wake_word_evt_data_t evtdata;
        evtdata.awaken_channel = (uint8_t)res->trigger_channel_id;
        m_callback(WAKE_EVT_AWAKEN, evtdata, m_user_data);
      }
    } else if (res->raw_data_channels > 1 &&
               res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
      wakeup_flag = 1;
      m_streaming_active = true;
      silence_frames = 0;
      uint32_t ch = (uint32_t)res->trigger_channel_id;
      LOGI_SYSTEM("Wake word detected on channel %d — streaming started", ch);
      // Change LED color to 100,0,100
      Board::getInstance().setAllLedsColor(100, 0, 100);
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::WAKE_WORD_DETECTED,
                                      ch);
      if (m_callback) {
        wake_word_evt_data_t evtdata;
        evtdata.awaken_channel = (uint8_t)ch;
        m_callback(WAKE_EVT_AWAKEN, evtdata, m_user_data);
      }
    }

    // ── VAD-based streaming timeout ─────────────────────────────────────────
    // While streaming: track silence. Reset timer on speech, stop on timeout.
    if (m_streaming_active) {
      if (res->vad_state == VAD_SPEECH) {
        silence_frames = 0; // voice activity — reset timer
      } else {
        silence_frames++;
        if (silence_frames >= SILENCE_TIMEOUT_FRAMES) {
          m_streaming_active = false;
          silence_frames = 0;
          wakeup_flag = 0;
          m_afe_handle->enable_wakenet(afe_data); // re-arm wake word
          uint32_t zero = 0;
          EventBus::getInstance().publish(APP_EVENTS, AppEvent::STOP_STREAMING,
                                          zero);
          LOGI_SYSTEM("VAD silence timeout (%d ms) — streaming stopped, "
                      "listening for wake word",
                      VAD_SILENCE_TIMEOUT_MS);
          // Change LED color to 0,0,0
          Board::getInstance().setAllLedsColor(0, 0, 0);
          if (m_callback) {
            wake_word_evt_data_t evtdata = {};
            m_callback(WAKE_EVT_CMD_TIMEOUT, evtdata, m_user_data);
          }
        }
      }
    }

    esp_task_wdt_reset();

    // ── Command recognition (only after wake word, if multinet loaded) ───────
    if (wakeup_flag == 1 && multinet && model_data) {
      esp_mn_state_t mn_state = multinet->detect(model_data, res->data);
      if (mn_state == ESP_MN_STATE_DETECTING)
        continue;

      if (mn_state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *mn_result = multinet->get_results(model_data);
        uint8_t cmd_id = (uint8_t)mn_result->command_id[0];
        LOGI_SYSTEM("Speech command: id=%d text=\"%s\"", cmd_id,
                    mn_result->string);
        wake_word_evt_data_t evtdata;
        evtdata.sr_cmd = cmd_id;
        if (m_callback)
          m_callback(WAKE_EVT_CMD, evtdata, m_user_data);
      }

      if (mn_state == ESP_MN_STATE_TIMEOUT) {
        // MultiNet timed out — VAD timeout will handle streaming stop
        ESP_LOGI(TAG, "MultiNet timeout");
      }
    }

  } // while (m_task_flag)

  // Cleanup
  if (model_data && multinet)
    multinet->destroy(model_data);

  ESP_LOGI(TAG, "detect task exiting");
  vTaskDelete(nullptr);
}
