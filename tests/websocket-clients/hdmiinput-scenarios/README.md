# HDMI Input simulation scenarios (YAML templates)

This folder contains YAML templates intended to be sent over the UT Control WebSocket YAML client
(`../python-client-send-yaml.py`) to simulate realistic HDMI Input source→sink behaviors.

## How to send a scenario

From `ut-control-17/tests/websocket-clients/`:

```bash
# Option A: copy (or edit) python-client-send-yaml.py to point at a scenario file
python3 python-client-send-yaml.py

# Option B (recommended): temporarily set file_name in python-client-send-yaml.py
# to one of the YAML files in ./hdmiinput-scenarios/
```

Notes:
- Each YAML file contains a single `hdmiinput:` command payload. If you need a multi-step
  scenario (sequence), send the individual files one-by-one in the order described below.
- Fields and enums used here match the existing `hdmiinput_*` command set:
  `connection_status`, `signal_status`, `hdcp_status`, `videoformat_change`,
  `audioinfo_frame`, `aviinfo_frame`, `spdinfo_frame`, `drminfo_frame`,
  `vsifinfo_frame`, `vrr_status`.

## Scenario templates

### 1) Hotplug connect (simple plug-in)
1. `hdmiinput_hotplug_connect.yaml`
2. `hdmiinput_signal_lock_1080p60.yaml`
3. `hdmiinput_hdcp2_authenticated.yaml`

### 2) Hotplug disconnect (unplug)
1. `hdmiinput_hotplug_disconnect.yaml`

### 3) Source switches resolution (1080p60 → 4Kp60)
1. `hdmiinput_videoformat_change_1080p60.yaml`
2. `hdmiinput_signal_unstable.yaml`
3. `hdmiinput_videoformat_change_4k60.yaml`
4. `hdmiinput_signal_lock_4k60.yaml`

### 4) HDCP authentication failure then recovery
1. `hdmiinput_hdcp_auth_in_progress.yaml`
2. `hdmiinput_hdcp_auth_failure.yaml`
3. `hdmiinput_hdcp2_authenticated.yaml`

### 5) VRR gaming source enable/disable
1. `hdmiinput_vrr_enable_120hz.yaml`
2. `hdmiinput_vrr_disable.yaml`

### 6) HDR10 content start/stop (DRM InfoFrame)
1. `hdmiinput_hdr10_drm_on.yaml`
2. `hdmiinput_hdr10_drm_off_sdr.yaml`

### 7) Audio format change (e.g., stereo PCM → multichannel)
1. `hdmiinput_audioinfo_pcm_2ch.yaml`
2. `hdmiinput_audioinfo_pcm_8ch.yaml`

### 8) Vendor Specific InfoFrame updates (VSIF)
1. `hdmiinput_vsif_generic_update.yaml`

### 9) SPD InfoFrame update (source/product string)
1. `hdmiinput_spd_bluray_player.yaml`
2. `hdmiinput_spd_game_console.yaml`

## Important
These are **templates** for simulation. The `data:` arrays are representative payloads consistent
with the existing examples, but the exact meaning of each byte is defined by the HDMI InfoFrame
specs (AVI, Audio, DRM, SPD, VSIF) and the receiver stack’s parsing behavior.
