# Optimization Directives: Gemini Live Zero-Allocation Steady State

This document contains explicit engineering directives for modifying `GeminiProtocolTask` and `AppController` within the `app/gemini_live/` framework. The goal is to eliminate dynamic memory allocations (`malloc`, `free`, `new`, `delete`) during the steady-state execution of audio streaming and tool execution loops.

---

## 🏛️ Summary of Core Objectives
1. **Move allocations to Boot-Time / Initialization:** Utilize class properties and static memory arenas inside your Singleton instance RAM boundaries.
2. **Eliminate Temporary String Copies:** Terminate incoming WebSocket network pointers in-place to bypass duplicate buffer creation loops.
3. **Prevent Memory Leaks Across Threads:** Switch from heap-allocated tracking objects to statically held data windows when publishing down the `EventBus` pool.

---

## 🛠️ Step-by-Step Implementation Tasks for the Agent

### Task 1: Update Class Definitions (`GeminiProtocolTask.h`)
Modify the private member layout of `GeminiProtocolTask` to maintain persistent, boot-allocated memory arenas.

1. Declare a continuous binary scratch arena for raw PCM decompression:
   ```cpp
   private:
       uint8_t* m_static_pcm_scratch_arena = nullptr;
       static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 24576; // 24KB max decoded output ceiling
   ```

2. Replace dynamic tool-handling allocations with a persistent, double-buffered tracking slot:
   ```cpp
   private:
       GeminiSkills::DecodedSkillCall m_static_skill_event_slot;
   ```

### Task 2: Refactor Constructor & Network Setup (`GeminiProtocolTask.cpp`)

1. In the constructor, allocate `m_static_pcm_scratch_arena` exactly once out of your external PSRAM pool, and ensure the pointer validation checks pass:
   ```cpp
   m_static_pcm_scratch_arena = static_cast<uint8_t*>(
       heap_caps_malloc(STATIC_PCM_ARENA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
   );
   assert(m_static_pcm_scratch_arena != nullptr);
   ```

2. Ensure `ws_cfg.buffer_size` inside `GeminiProtocolTask::run()` is updated to `32768` (32KB) to comfortably swallow large base64 JSON payload packets dropping down the WebSocket channel without overrunning internal TCP frames.

### Task 3: Refactor Payload Extraction (`GeminiProtocolTask.cpp`)

Modify `processIncomingFrame(char* payload, size_t length)` to enforce zero-allocation logic.

1. **In-Place String Termination Optimization:**
Bypass the expensive `std::string frame(payload, length);` allocation layout. Instead, capture the character at the offset bound, temporarily swap it with a null-terminator `\0`, run `cJSON_Parse`, and restore the character immediately before exiting the function context.

2. **Audio Decoding Optimization:**
Inside your `inlineData` Base64 decoding check block, ensure `mbedtls_base64_decode` passes your pre-allocated `m_static_pcm_scratch_arena` directly as the output target.
* Remove the dynamic `heap_caps_malloc` / `heap_caps_free` sequence completely.
* Add a boundary guard: `if (pcm_out_len > 0 && pcm_out_len <= STATIC_PCM_ARENA_MAX_SIZE)`.

3. **Tool Call Optimization:**
When an active `toolCall` registers, remove `auto* skill_data = new GeminiSkills::DecodedSkillCall();` entirely.
* Wipe your persistent internal member variable cleanly: `std::memset(&m_static_skill_event_slot, 0, sizeof(m_static_skill_event_slot));`
* Invoke your auto-generated decoder directly onto it: `GeminiSkills::decode_incoming_arguments(..., m_static_skill_event_slot)`
* Broadcast the fixed reference address directly across the interface line:
   ```cpp
   EventBus::getInstance().publish(APP_EVENTS, AppEvent::GEMINI_TOOL_CALL, &m_static_skill_event_slot);
   ```

### Task 4: Streamline Consumer Cleanup (`AppController.cpp`)

Since the data published under `AppEvent::GEMINI_TOOL_CALL` is now bound to a persistent static memory instance slot within `GeminiProtocolTask` instead of a heap-allocated pointer, you must alter the resource management logic inside `AppController::onEvent`:

1. Strip out and **delete** all legacy occurrences of `delete skill_call;` or `heap_caps_free(skill_call);` at the end of the execution block.
2. Because the data is evaluated synchronously within your event dispatcher thread cycle, the data properties can be extracted and modified safely without any risks of memory leak compilation errors or runtime pointer dangling bugs.
