#pragma once

#include "common/hw_types.h"
#include "common/system_settings.h"

/**
 * @brief Application-wide singleton context.
 *
 * Owns the system configuration (loaded from Kconfig) and hardware handles
 * (populated by Board::begin()). All services access settings and handles
 * through this object instead of through raw pointers passed between functions.
 *
 * Lifecycle:
 *   1. Call SystemContext::get().init() early in app_main — before Board::begin().
 *   2. Board::begin() populates hw after hardware init.
 *   3. Every service calls SystemContext::get() to read what it needs.
 *
 * Thread safety: settings are written once at startup and then read-only.
 * hw handles are written once by Board::begin() and then read-only.
 */
class SystemContext {
public:
  /** @brief Access the singleton instance. */
  static SystemContext &get();

  /**
   * @brief Load settings from Kconfig compile-time defaults.
   *        Call once, before Board::begin().
   */
  void init();

  // ---- Owned data --------------------------------------------------------
  GlobalSystemSettings settings; ///< Loaded from Kconfig in init()
  HardwareAudioHandles hw;       ///< Populated by Board after begin()

private:
  SystemContext() = default;
  ~SystemContext() = default;
  SystemContext(const SystemContext &) = delete;
  SystemContext &operator=(const SystemContext &) = delete;
};
