import sys
from pathlib import Path

# Ensure host_tests/build is on sys.path so waveshare_host can be imported
build_dir = Path(__file__).parent / "build"
if str(build_dir) not in sys.path:
    sys.path.insert(0, str(build_dir))

import pytest
import waveshare_host as wh

@pytest.fixture
def sysdb():
    """Provides the singleton EmbeddedSysDb instance with clean defaults."""
    db = wh.EmbeddedSysDb.get_instance()
    # Reset critical fields to defaults
    def reset(s):
        s.speaker_volume = 80
        s.mic_gain_db = 60.0
        s.mic_enabled = True
        s.wifi_connected = False
        s.assistant_speaking = False
        s.turn_complete_pending = False
        s.companion_connected = False
        s.companion_settled = False
        s.session_state = wh.AssistantState.Idle
        s.pipeline_mode = wh.PipelineMode.WAKE_IDLE
        s.media_state = wh.MediaPlaybackState.IDLE
        s.is_ducked = False
        s.autoplay_enabled = True
        s.active_song_id = ""
        s.media_title = ""
    db.mutate(reset)
    return db

@pytest.fixture
def buffer_manager():
    """Provides the singleton BufferManager instance."""
    bm = wh.BufferManager.get_instance()
    bm.init_all()
    return bm
