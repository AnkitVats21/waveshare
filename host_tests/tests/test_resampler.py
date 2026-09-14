import waveshare_host as wh


def test_compute_resampled_frames_basic():
    assert wh.compute_resampled_frames(100, 16000, 16000) == 100
    assert wh.compute_resampled_frames(100, 16000, 32000) == 200
    assert wh.compute_resampled_frames(100, 32000, 16000) == 50


def test_compute_resampled_frames_zero_src_rate():
    assert wh.compute_resampled_frames(100, 0, 16000) == 0


def test_resample_identity_when_rates_match():
    r = wh.LinearResampler()
    src = [100, -200, 300, -400]
    dst = r.resample(src, dst_frames=4, channels=1)
    assert dst == src


def test_resample_upsample_doubles_length_and_interpolates():
    r = wh.LinearResampler()
    src = [0, 100]
    dst = r.resample(src, dst_frames=4, channels=1)
    assert len(dst) == 4
    assert dst[0] == 0
    assert dst[-1] == 100
    # Interior samples should be monotonically interpolated, not just repeated.
    assert dst[0] <= dst[1] <= dst[2] <= dst[3]


def test_resample_downsample_halves_length():
    r = wh.LinearResampler()
    src = [0, 10, 20, 30]
    dst = r.resample(src, dst_frames=2, channels=1)
    assert len(dst) == 2
    assert dst[0] == 0


def test_resample_stereo_preserves_channel_interleaving():
    r = wh.LinearResampler()
    # Two stereo frames: (L=0,R=1000), (L=100,R=1100)
    src = [0, 1000, 100, 1100]
    dst = r.resample(src, dst_frames=2, channels=2)
    assert len(dst) == 4
    assert dst[0] == 0
    assert dst[1] == 1000
    assert dst[2] == 100
    assert dst[3] == 1100


def test_resample_zero_frames_is_noop():
    r = wh.LinearResampler()
    assert r.resample([], dst_frames=4, channels=1) == [0, 0, 0, 0]
    assert r.resample([1, 2, 3], dst_frames=0, channels=1) == []
