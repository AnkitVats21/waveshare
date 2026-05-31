#pragma once

#include "RtpTaskBase.h"
#include "common/system_settings.h"
#include "esp_event_base.h"

/**
 * @brief RTP Streamer for transmitting audio data over UDP
 */
class RtpStreamer : public RtpTaskBase {
public:
#pragma pack(push, 1)
  struct rtp_header_t {
    uint8_t version_p_x_cc;
    uint8_t payload_type;
    uint16_t sequence_num;
    uint32_t timestamp;
    uint32_t ssrc;
  };
#pragma pack(pop)

  struct TxConfig : public CommonConfig {
    std::string target_ip;
    uint8_t payload_type = 96; // 96 = Dynamic PCM L16
    AudioStreamFormat format = AudioStreamFormat::PCM_S16LE;
  };

  RtpStreamer(const TxConfig &config, BufferManager::BufferId buf_id, int shared_socket);

  /**
   * @brief Event handler bridge for receiving notifications from the EventBus
   */
  static void eventHandlerBridge(void *handler_arg, esp_event_base_t base,
                                 int32_t id, void *event_data);

protected:
  void processLoop() override;

private:
  TxConfig m_tx_config;
};
