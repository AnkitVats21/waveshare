# hub_link

The board's side of a LAN hub (nexus-hub): built with
`CONFIG_WAVESHARE_VOICE_HUB`.

- **`HubLink`**: one WebSocket to the hub (found by mDNS
  `_nexus-hub._tcp`, or the `hub_url` setting), hello and roles, reconnects.
  It carries the dashboard's relay (`IHubRelay`, implemented by main's
  `ControlChannel`) so the dashboard reaches the board through the hub.
- **`HubAgent`**: the voice backend through the hub (a `VoiceAgent`): mic
  audio up as Opus (`UplinkEncoder`, `UplinkFraming`, host-tested) or PCM,
  reply audio down, tool calls the hub forwards to the board's registry. It
  tells `VoiceSession` what the hub did (a music command) and counts a hub
  tool as running from `reply_owed` until its answer plays.
- **`HubMusic`**: the board as one of the hub's music players.

main wires it: `HubLink::setRelay()`, `HubLink::setSettingsSource()` (the hub
URL and uplink Opus settings from system.ndb), `HubAgent::setVoiceSession()`.
