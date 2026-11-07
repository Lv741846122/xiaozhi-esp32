#include "emote_display.h"

// Standard C++ headers
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <tuple>
#include <unordered_map>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <driver/temperature_sensor.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "expression_emote.h"
#include "gfx.h"
#include "grok_ball_overlay.h"

namespace emote {

// ESP32-S3 internal temperature sensor. It reports die temperature, not room
// air temperature, but it is the only on-board source without extra hardware.
static temperature_sensor_handle_t s_temp_sensor = nullptr;
static bool s_temp_sensor_tried = false;

static void EnsureTemperatureSensor() {
    if (s_temp_sensor_tried) {
        return;
    }
    s_temp_sensor_tried = true;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&cfg, &s_temp_sensor) != ESP_OK) {
        s_temp_sensor = nullptr;
        return;
    }
    if (temperature_sensor_enable(s_temp_sensor) != ESP_OK) {
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = nullptr;
    }
}

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t* const edata, void* user_ctx) {
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush bookkeeping, so that a display which has stopped flushing can be noticed.
//
// Nothing in the tree watches the render task: no task is subscribed to the task watchdog and
// CONFIG_ESP_TASK_WDT_PANIC is off, and a task blocked on an event group yields the CPU, so the
// idle-task watchdog sees nothing wrong. These two values are the minimum a board needs to tell
// a live display from a wedged one.
static std::atomic<uint32_t> s_flush_count{0};
static std::atomic<int64_t> s_last_flush_us{0};

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data,
                            emote_handle_t handle) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);

    // Every exit from here must end in a flush-finished report, one way or the other.
    //
    // The renderer clears the flush-done bit, calls this, then waits on that bit with
    // portMAX_DELAY (esp_emote_gfx, gfx_render.c) - and the only thing that sets it is
    // emote_notify_flush_finished, which is reached only from the panel's on_color_trans_done
    // callback. So a block that never reaches the bus is not a dropped frame: the render task
    // waits for it forever, holding the graphics lock, and every caller of gfx_emote_lock
    // queues up behind it. The device keeps answering pings and WebSocket pongs the whole
    // time, because those are other tasks - so it looks alive and the screen is simply frozen
    // on one frame. Reporting completion here is safe: the bit is set before the renderer
    // reaches its wait, and a pre-set bit makes that wait return at once.
    if (panel == nullptr) {
        emote_notify_flush_finished(handle);
        return;
    }

    // Replace the stock colour emote with a black-background / white-stroke
    // Grok Ball. Only the middle band is touched: the status bar above and the
    // caption strip below keep whatever the emote engine rendered.
    GrokApplyToBlock(x_start, y_start, x_end, y_end, data);

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed (%s) for (%d,%d)-(%d,%d); reporting flush done so the "
                      "render task is not left waiting for a transfer that never started",
                 esp_err_to_name(err), x_start, y_start, x_end, y_end);
        emote_notify_flush_finished(handle);
        return;
    }

    uint32_t flush_no = s_flush_count.fetch_add(1, std::memory_order_relaxed) + 1;
    s_last_flush_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    // Log the first frame and then a heartbeat, so a silent screen can be told
    // apart from a renderer that never sends anything to the panel.
    if (flush_no == 1 || flush_no % 150 == 0) {
        ESP_LOGI(TAG, "flush #%lu (%d,%d)-(%d,%d) data=%p",
                 static_cast<unsigned long>(flush_no), x_start, y_start, x_end, y_end, data);
    }
}

uint32_t GetFlushCount() {
    return s_flush_count.load(std::memory_order_relaxed);
}

int64_t GetMicrosecondsSinceLastFlush() {
    int64_t last = s_last_flush_us.load(std::memory_order_relaxed);
    if (last == 0) {
        return -1;  // nothing has been flushed yet; not the same as "a long time ago"
    }
    return esp_timer_get_time() - last;
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width,
                                      const int height) {
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags =
            {
                .swap = true,
                .double_buffer = true,
                .buff_dma = false,
            },
        .gfx_emote =
            {
                .h_res = width,
                .v_res = height,
                .fps = 30,
            },
        .buffers =
            {
                .buf_pixels = static_cast<size_t>(width * 16),
            },
        .task =
            {
                .task_priority = 5,
                .task_stack = 6 * 1024,
                .task_affinity = 0,
                .task_stack_in_ext = false,
            },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

void EmoteDisplay::OnAnimTimer(void* arg) {
    auto* self = static_cast<EmoteDisplay*>(arg);
    if (self == nullptr) {
        return;
    }
    // Compute the pose ONCE per frame, then ask the engine to repaint.
    // All 16-row flush blocks of that frame read the cached pose.
    GrokUpdatePose(esp_timer_get_time());

    // Refresh the fixed status bar once a second (12 ticks x 83ms).
    static int s_tick = 0;
    if (++s_tick >= 12) {
        s_tick = 0;
        EnsureTemperatureSensor();
        time_t now = 0;
        struct tm timeinfo = {};
        time(&now);
        localtime_r(&now, &timeinfo);
        float celsius = 0.0f;
        if (s_temp_sensor != nullptr &&
            temperature_sensor_get_celsius(s_temp_sensor, &celsius) != ESP_OK) {
            celsius = 0.0f;
        }
        int level = 100;
        bool charging = false, discharging = false;
        Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        GrokSetStatus(timeinfo.tm_hour, timeinfo.tm_min,
                      static_cast<int>(celsius), level, charging);
    }
    self->RefreshAll();
}

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel,
                           const esp_lcd_panel_io_handle_t panel_io, const int width,
                           const int height) {
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);

    // 12fps full-frame nudge. Once idle the engine only repaints a narrow
    // 17-row strip, which would leave the Grok Ball frozen.
    const esp_timer_create_args_t timer_args = {
        .callback = &EmoteDisplay::OnAnimTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "grok_anim",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &anim_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(anim_timer_, 83 * 1000));
}

EmoteDisplay::~EmoteDisplay() {
    if (anim_timer_ != nullptr) {
        esp_timer_stop(anim_timer_);
        esp_timer_delete(anim_timer_);
        anim_timer_ = nullptr;
    }
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

bool EmoteDisplay::SetEmotion(const char* const emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    // Pick the Grok Ball eye ring for this emotion (server-driven or local).
    GrokSetEmotion(emotion);
    if (emote_handle_ == nullptr || emotion == nullptr || strlen(emotion) == 0) {
        return false;
    }
    // Push the new eye ring to the panel immediately, even when the EAF
    // animation is left untouched below.
    RefreshAll();
    // The server sends emotion names the shipped EAF pack does not carry
    // (e.g. "robot_2"). Asking the engine for them produces a hard error and
    // stops the repaint loop, so fall back to a stock animation while the
    // Grok Ball overlay renders the real emotion-selected eye rings.
    if (GrokRingForName(emotion) < 0) {
        emote_set_anim_emoji(emote_handle_, "neutral");
        return true;
    }
    // The result was already being produced and discarded. An emotion the mounted asset pack
    // does not carry fails inside emote_set_emoji_animation, and callers had no way to find
    // out: an MCP tool, a server-driven emotion or a board's own avatar mapping all saw the
    // same silence as a success.
    esp_err_t err = emote_set_anim_emoji(emote_handle_, emotion);
    if (err != ESP_OK) {
        // An emotion the asset pack does not carry (the "gear" status icon is one)
        // used to leave the canvas with nothing to draw, which looks exactly like a
        // dead screen. Fall back to a stock emotion so the display still renders.
        static const char* const kFallbacks[] = {"neutral", "Neutral", "happy", "Happy"};
        for (const char* fb : kFallbacks) {
            if (strcmp(fb, emotion) == 0) {
                continue;
            }
            if (emote_set_anim_emoji(emote_handle_, fb) == ESP_OK) {
                ESP_LOGW(TAG, "emotion \"%s\" unavailable, fell back to \"%s\"", emotion, fb);
                return true;
            }
        }
        ESP_LOGW(TAG, "emotion \"%s\" not set: %s", emotion, esp_err_to_name(err));
        return false;
    }
    return true;
}

void EmoteDisplay::ApplyPendingUserText() {
    if (emote_handle_ == nullptr || pending_user_text_.empty()) {
        return;
    }
    gfx_obj_t* toast = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_TOAST_LABEL);
    if (toast == nullptr) {
        ESP_LOGW(TAG, "toast_label not found; STT caption cannot be shown");
        return;
    }
    if (emote_lock(emote_handle_) != ESP_OK) {
        return;
    }
    gfx_label_set_text(toast, pending_user_text_.c_str());
    gfx_obj_set_visible(toast, true);
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content) {
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (emote_handle_ == nullptr || role == nullptr || content == nullptr) {
        return;
    }

    // "user" is the live speech-to-text transcript. emote_set_event_msg() hides
    // every UI element before running the handler, and EVT_LISTEN discards its
    // message entirely - so while the state machine flips between listening
    // and speaking, a transcript pushed via EVT_SPEAK disappears at once.
    // Write it directly to the toast label and re-apply it after status changes.
    if (std::strcmp(role, "user") == 0) {
        if (content[0] == '\0') {
            pending_user_text_.clear();
            gfx_obj_t* toast = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_TOAST_LABEL);
            if (toast) {
                emote_lock(emote_handle_);
                gfx_label_set_text(toast, "");
                gfx_obj_set_visible(toast, false);
                emote_unlock(emote_handle_);
            }
            return;
        }
        pending_user_text_ = content;
        ApplyPendingUserText();
        return;
    }

    if (content[0] == '\0') {
        return;
    }
    // Any assistant/system message replaces the transcript.
    pending_user_text_.clear();

    if ((std::strcmp(role, "system") == 0) && std::strstr(content, "xiaozhi.me")) {
        size_t len = strlen(content);
        char* new_content = new char[len + 1];
        strcpy(new_content, content);
        std::replace(new_content, new_content + len, static_cast<char>(0x0A),
                     static_cast<char>(0x20));
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
        delete[] new_content;
    } else {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
    }
}

void EmoteDisplay::SetStatus(const char* const status) {
    ESP_LOGI(TAG, "SetStatus: %s", status);
    // Drive the Grok Ball talking rhythm for both listening and speaking.
    // Idle breathing / blinking runs regardless of this flag.
    GrokSetActive(status != nullptr &&
        (std::strcmp(status, Lang::Strings::LISTENING) == 0 ||
         std::strcmp(status, Lang::Strings::SPEAKING) == 0));
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        }
        // The events above hide the toast label; keep the transcript visible.
        ApplyPendingUserText();
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all) {
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on) {
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image) {
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

bool EmoteDisplay::MountAssets(const char* partition_label) {
    if (emote_handle_ == nullptr || partition_label == nullptr) {
        return false;
    }
    const emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source =
            {
                .partition_label = partition_label,
            },
        .flags =
            {
                .mmap_enable = true,
            },
    };
    return emote_mount_assets(emote_handle_, &data) == ESP_OK;
}

void EmoteDisplay::UnmountAssets() {
    if (emote_handle_ != nullptr) {
        emote_unmount_assets(emote_handle_);
    }
}

bool EmoteDisplay::GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
    if (emote_handle_ == nullptr) {
        return false;
    }
    return emote_get_asset_data_by_name(emote_handle_, name.c_str(), &data, &size) == ESP_OK;
}

void EmoteDisplay::LoadAssets() {
    if (emote_handle_ == nullptr) {
        return;
    }
    emote_load_assets(emote_handle_);
    // Keep a looping EAF animation alive. The engine only repaints the strip
    // where its own animation object sits; without this the middle band goes
    // static and the Grok Ball overlay never gets a chance to animate.
    emote_set_anim_emoji(emote_handle_, "neutral");
    // The shipped 320_240 layout hard-codes toast_label at x=25 / width=280
    // and status_icon at x=-145. Both are off-centre or clipped on a 240px
    // wide portrait panel, so re-centre them once the objects exist.
    if (emote_lock(emote_handle_) != ESP_OK) {
        return;
    }
    gfx_obj_t* toast = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_TOAST_LABEL);
    if (toast != nullptr) {
        // Move captions out of the top row: a fixed status bar lives there
        // now. One text row plus a small gap below the ball.
        gfx_obj_set_size(toast, 224, 26);
        gfx_obj_align(toast, GFX_ALIGN_BOTTOM_MID, 0, -16);
    }
    // The EAF status icon / clock / battery objects are anchored at the top
    // by the shipped layout. The overlay repaints y<34 completely, so they
    // are simply covered; no need to fight their timers here.
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool EmoteDisplay::Lock(const int timeout_ms) {
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock() {}

bool EmoteDisplay::StopAnimDialog() {
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        // esp_err_t, where ESP_OK is 0: returned as bool it reports false on success and true on
        // failure. GetAssetData and SetEmotion compare against ESP_OK for the same reason.
        return emote_stop_anim_dialog(emote_handle_) == ESP_OK;
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms) {
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms) == ESP_OK;
    }
    return false;
}

void EmoteDisplay::RefreshAll() {
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

}  // namespace emote
