# Device-profile work

- Read [README.md](README.md) before creating or migrating a profile.
- Keep application/provider URL lists, NTP hostnames and device Wi-Fi names in
  profile data. Use generic emulator properties and the shared compiler.
- Preserve hardware-only schema-1 profiles and offline defaults.
- Validate the profile with `tools/esp_board.py` and run the checks listed in the
  README. Network behavior changes require the actual packet regression.
- Keep profile/device names descriptive; they may identify the application.
- Commit each complete change separately and preserve unrelated working files.
