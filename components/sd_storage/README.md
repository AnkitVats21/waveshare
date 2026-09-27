# sd_storage

The SD card: mounting, file handles and the rules for paths that come from
outside the firmware. Everything that touches the card goes through here.

## What's here

- **`SdCard`** — mounts FATFS over SDMMC with wiring supplied by the board
  layer. Owns an optional DMA bounce buffer for transfers from PSRAM
  (`CONFIG_SD_STORAGE_DMA_BUFFER_SIZE`).
- **`File`** — RAII handle over a POSIX file descriptor (no stdio: FATFS
  already caches sectors per open file). Opens are checked against every
  other open `File` on the same path (`OpenFileTable`): a second writer, or
  a reader against a writer, waits up to a timeout and then fails. A reader
  opened with `Share::FollowWriter` may read a file still being written
  (the music cache reader tails the `.tmp` being downloaded). Buffers the SDMMC DMA can't reach (RTC RAM, flash) are copied
  through PSRAM rather than handed to the driver.
- **`Fs`** — whole-file and directory operations: `readText`, `readHead`,
  `writeAtomic` (write `.tmp`, fsync, swap; a power cut leaves the old or
  the new file), `remove`, `rename`, `mkdirs`, `list`, `stat`.
  **`remove` and `rename` fail while the file is open.** FATFS's own
  locking is off (`CONFIG_FATFS_FS_LOCK=0`); this table replaces it.
- **`PathPolicy`** — for paths from the HTTP file API and the Gemini file
  tools: normalises separators, rejects `..`, roots the path under the
  mount point, and refuses the protected files (the credential files, list
  owned by the caller).

## Notes

- The card is on core 0 with the other I/O (`CORE_STORAGE`).
- SD corruption under streaming load was traced to DMA from RTC fast RAM
  (it read zeros); RTC RAM is no longer in the heap, and `File` copies such
  buffers anyway.
