// =============================================================================
// STAR Schema Definition: sysdb.star
// Single-writer Tagged-field Asynchronous Replication State Database Schema
// =============================================================================

enum MediaPlaybackState : uint8_t {
    IDLE = 0,
    RESOLVING = 1,
    BUFFERING = 2,
    PLAYING = 3,
    PAUSED = 4,
    ERROR_STATE = 5
}

component System id=0 mask=0x00010000 {
    field wifi_connected: bool = false [readonly, bit=0:WIFI_CONNECTED]
    field network_state: NetworkState = NetworkState::Disconnected [readonly, bit=2:NETWORK_STATE]
    field ap_active: bool = false [readonly, bit=3:AP_ACTIVE]
    field wifi_ssid: string[33] = "" [writable, bit=4:WIFI_CONFIG]
    field wifi_password: string[65] = "" [writable, bit=4:WIFI_CONFIG]
    field wifi_apply_creds: bool = false [writable, bit=5:APPLY_CREDS]
}

component Audio id=1 mask=0x00020000 {
    field sample_rate: uint32_t = LOCAL_SAMPLE_RATE [readonly, bit=0:SAMPLE_RATE]
    field mic_gain_db: float = 60.0f [writable, bit=1:MIC_GAIN]
    field speaker_volume: int = 80 [writable, bit=2:SPEAKER_VOLUME]
    field mic_enabled: bool = true [writable, bit=3:MIC_ENABLED]
    field assistant_speaking: bool = false [readonly, bit=4:ASST_SPEAKING]
    field session_active: bool = false [readonly, bit=5:SESSION_ACTIVE]
    field turn_complete_pending: bool = false [readonly, bit=6:TURN_COMPLETE]
    field record_all_mic_channels: bool = true [writable, bit=10:RECORD_CHANNELS]
}

component Pipeline id=2 mask=0x00040000 {
    field mode: PipelineMode = PipelineMode::WAKE_IDLE [writable, bit=0:MODE]
}

component Assistant id=3 mask=0x00080000 {
    field session_state: AssistantState = AssistantState::Idle [readonly, bit=0:SESSION_STATE]
    field visual_state: AssistantVisualState = AssistantVisualState::Offline [readonly, bit=1:VISUAL_STATE]
    field connect_requested: bool = false [writable, bit=3:CONNECT_REQ]
    field ws_state: WsState = WsState::DISCONNECTED [readonly, bit=2:WS_STATE]
    field media_pending_idle: bool = false [readonly]
}

component Led id=4 mask=0x00100000 {
    field mode: LedMode = LedMode::OFF [writable, bit=0:MODE]
    field color: RgbColor = OFF_LED [writable, bit=1:COLOR, color]
    field speed_ms: uint32_t = 500 [writable, bit=2:SPEED]
    field repeat: uint8_t = 0 [writable]
}

component Alarm id=6 mask=0x00400000 {
    field playing: bool = false [readonly, bit=0:PLAYING]
    field stop_requested: bool = false [writable, bit=1:STOP_REQUESTED]
    field active_alarm_id: int = 0 [writable]
}

component Bluetooth id=7 mask=0x00800000 {
    alias READY = CONNECTED
    field connected: bool = false [piorigin, bit=0:CONNECTED]
    field device_name: string[32] = "" [piorigin, bit=1:DEVICE_NAME]
    field mac_address: string[18] = "" [piorigin, bit=2:MAC_ADDRESS]
    field rssi: int8_t = 0 [piorigin, bit=3:STATUS]
}

component Media id=8 mask=0x01000000 {
    field state: MediaPlaybackState = MediaPlaybackState::IDLE [writable, bit=0:STATE]
    field active_song_id: string[64] = "" [writable, bit=1:TRACK]
    field title: string[64] = "" [writable, bit=1:TRACK]
    field artist: string[64] = "" [writable, bit=1:TRACK]
    field position_ms: uint32_t = 0 [writable, bit=6:POSITION]
    field duration_ms: uint32_t = 0 [writable, bit=7:DURATION]
    field seekable: bool = false [readonly]
    field repeat_mode: uint8_t = 0 [writable, bit=2:REPEAT]
    field is_ducked: bool = false [readonly, bit=3:DUCKED]
    field autoplay_enabled: bool = true [writable, bit=4:AUTOPLAY]
    field cache_downloads: bool = false [writable, bit=5:CACHE]
    field output_target: MediaOutputTarget = MediaOutputTarget::LOCAL [readonly, bit=8:TARGET]
    field pending_command: MediaPendingCommand = {} [writable, bit=9:CMD]
}
