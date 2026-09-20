# vendor/core_sysdb

These headers are copied from the `waveshare` firmware repo's
`components/core_sysdb/include/core_sysdb/`. They define the STAR wire
protocol and generated system-state schema shared between the ESP32
firmware and this daemon, so the two sides must agree on their layout.

`SystemState.generated.h` is produced by `tools/starc/starc.py` from
`schema/sysdb.star` in the firmware repo. When that schema changes,
regenerate it there and copy the updated files here:

```
core_sysdb/SystemState.h
core_sysdb/SystemState.generated.h
core_sysdb/app_types.h
core_sysdb/led_types.h
core_sysdb/WalTypes.h
core_sysdb/AudioRates.h
core_sysdb/StarProtocol.h
common/led_types.h        (forwards to core_sysdb/led_types.h)
```

Do not edit these files directly in this repo — changes belong in the
firmware repo's schema/headers and should be re-vendored.
