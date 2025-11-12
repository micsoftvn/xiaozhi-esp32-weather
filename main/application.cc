#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

static std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c);
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}

static std::string CapitalizeWords(const std::string& text) {
    std::string result = text;
    bool new_word = true;
    for (auto& ch : result) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            new_word = true;
        } else {
            if (new_word) {
                ch = std::toupper(static_cast<unsigned char>(ch));
                new_word = false;
            } else {
                ch = std::tolower(static_cast<unsigned char>(ch));
            }
        }
    }
    return result;
}

static constexpr const char* kDefaultWeatherCity = "Hanoi";
static constexpr const char* kDefaultWeatherApiKey = "fbf5a0e942e6fea3ff18103b9fd46ed9";
static constexpr std::chrono::minutes kWeatherSuccessTtl{30};
static constexpr std::chrono::minutes kWeatherRetryInterval{5};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_frame = [this](const std::vector<int16_t>& pcm) {
        audio_player_.OnPlaybackFrame(pcm);
    };
    audio_service_.SetCallbacks(callbacks);

#ifdef SDCARD_MOUNT_POINT
    const char* mount_point = SDCARD_MOUNT_POINT;
#else
    const char* mount_point = "/sdcard";
#endif
    audio_player_.Initialize(mount_point, &audio_service_, display);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);
    RequestWeatherUpdate(true);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            UpdateIdleDisplay();
            display->UpdateStatusBar();
            if (clock_ticks_ % 60 == 0) {
                RequestWeatherUpdate(false);
            }
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    if (state != kDeviceStateIdle) {
        display->HideIdleCard();
    }
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            RequestWeatherUpdate(false);
            UpdateIdleDisplay();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [display](int progress, size_t speed) {
        std::thread([display, progress, speed]() {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            display->SetChatMessage("system", buffer);
        }).detach();
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::StopAudioPlayback() {
    audio_player_.Stop();
}

std::string Application::GetIdleStatusText() {
    char time_str[16] = "";
    bool time_valid = false;
    time_t now = time(nullptr);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) != nullptr && tm_buf.tm_year >= 2025 - 1900) {
        strftime(time_str, sizeof(time_str), "%H:%M", &tm_buf);
        time_valid = true;
    } else {
        ESP_LOGW(TAG, "System time is not set correctly for idle status");
    }

    std::string status = time_valid ? std::string(time_str) : std::string();
    std::string weather_summary;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_available_) {
            weather_summary = FormatWeatherSummary(weather_info_);
        }
    }

    if (!weather_summary.empty()) {
        if (!status.empty()) {
            status.append("  ");
        }
        status.append(weather_summary);
    }

    if (status.empty()) {
        status = time_valid ? std::string(time_str) : std::string(Lang::Strings::STANDBY);
    }
    return status;
}

void Application::RequestWeatherUpdate(bool force) {
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_fetch_in_progress_) {
            if (force) {
                ESP_LOGW(TAG, "Weather update already running");
            }
            return;
        }
        if (!force) {
            if (weather_last_request_ != std::chrono::steady_clock::time_point{} &&
                now - weather_last_request_ < kWeatherRetryInterval) {
                return;
            }
            if (weather_available_ &&
                weather_last_success_ != std::chrono::steady_clock::time_point{} &&
                now - weather_last_success_ < kWeatherSuccessTtl) {
                return;
            }
        }
        weather_fetch_in_progress_ = true;
        weather_last_request_ = now;
    }

    ESP_LOGI(TAG, "Scheduling weather update");
    BaseType_t created = xTaskCreate([](void* arg) {
        static_cast<Application*>(arg)->FetchWeatherTask();
        vTaskDelete(nullptr);
    }, "weather_fetch", 6144, this, 3, nullptr);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create weather fetch task");
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_fetch_in_progress_ = false;
    }
}

void Application::FetchWeatherTask() {
    WeatherInfo info;
    bool success = FetchWeatherData(info);
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_fetch_in_progress_ = false;
        if (success) {
            weather_info_ = std::move(info);
            weather_available_ = true;
            weather_last_success_ = std::chrono::steady_clock::now();
        }
    }

    if (success) {
        ESP_LOGI(TAG, "Weather updated successfully");
        Schedule([this]() {
            if (device_state_ == kDeviceStateIdle) {
                UpdateIdleDisplay();
            }
        });
    }
}

bool Application::FetchWeatherData(WeatherInfo& info) {
    Settings weather_settings("weather", false);
    std::string city = weather_settings.GetString("city");
    if (city.empty()) {
        city = kDefaultWeatherCity;
    }
    std::string api_key = weather_settings.GetString("api_key");
    if (api_key.empty()) {
        api_key = kDefaultWeatherApiKey;
    }

    auto& board = Board::GetInstance();
    auto http = board.GetNetwork()->CreateHttp(5);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "xiaozhi-weather/1.0");

    std::string url = "https://api.openweathermap.org/data/2.5/weather?q=" + UrlEncode(city) +
        "&appid=" + api_key + "&units=metric&lang=en";

    ESP_LOGI(TAG, "Fetching weather from %s", url.c_str());
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open weather URL");
        return false;
    }

    int status_code = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();

    if (status_code != 200) {
        ESP_LOGE(TAG, "Weather request failed with status %d", status_code);
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse weather response JSON");
        return false;
    }

    bool success = false;
    do {
        cJSON* name = cJSON_GetObjectItem(root, "name");
        if (!cJSON_IsString(name)) {
            break;
        }
        cJSON* main_obj = cJSON_GetObjectItem(root, "main");
        if (!cJSON_IsObject(main_obj)) {
            break;
        }
        cJSON* temp = cJSON_GetObjectItem(main_obj, "temp");
        if (!cJSON_IsNumber(temp)) {
            break;
        }
        cJSON* humidity = cJSON_GetObjectItem(main_obj, "humidity");
        cJSON* feels_like = cJSON_GetObjectItem(main_obj, "feels_like");
        cJSON* pressure = cJSON_GetObjectItem(main_obj, "pressure");
        cJSON* temp_min = cJSON_GetObjectItem(main_obj, "temp_min");
        cJSON* temp_max = cJSON_GetObjectItem(main_obj, "temp_max");

        cJSON* wind_obj = cJSON_GetObjectItem(root, "wind");
        cJSON* wind_speed = nullptr;
        cJSON* wind_deg = nullptr;
        if (cJSON_IsObject(wind_obj)) {
            wind_speed = cJSON_GetObjectItem(wind_obj, "speed");
            wind_deg = cJSON_GetObjectItem(wind_obj, "deg");
        }

        cJSON* sys_obj = cJSON_GetObjectItem(root, "sys");
        cJSON* sunrise = nullptr;
        cJSON* sunset = nullptr;
        if (cJSON_IsObject(sys_obj)) {
            sunrise = cJSON_GetObjectItem(sys_obj, "sunrise");
            sunset = cJSON_GetObjectItem(sys_obj, "sunset");
        }

        cJSON* weather_array = cJSON_GetObjectItem(root, "weather");
        if (!cJSON_IsArray(weather_array) || cJSON_GetArraySize(weather_array) == 0) {
            break;
        }
        cJSON* weather0 = cJSON_GetArrayItem(weather_array, 0);
        if (!cJSON_IsObject(weather0)) {
            break;
        }
        cJSON* description = cJSON_GetObjectItem(weather0, "description");
        cJSON* icon = cJSON_GetObjectItem(weather0, "icon");

        info.city = name->valuestring;
        info.temperature_c = static_cast<float>(temp->valuedouble);
        info.feels_like_c = cJSON_IsNumber(feels_like) ? static_cast<float>(feels_like->valuedouble) : info.temperature_c;
        info.humidity = cJSON_IsNumber(humidity) ? humidity->valueint : 0;
        info.wind_speed = wind_speed && cJSON_IsNumber(wind_speed) ? static_cast<float>(wind_speed->valuedouble) : 0.0f;
        info.wind_deg = wind_deg && cJSON_IsNumber(wind_deg) ? wind_deg->valueint : -1;
        info.pressure = pressure && cJSON_IsNumber(pressure) ? pressure->valueint : 0;
        info.temp_min_c = temp_min && cJSON_IsNumber(temp_min) ? static_cast<float>(temp_min->valuedouble) : info.temperature_c;
        info.temp_max_c = temp_max && cJSON_IsNumber(temp_max) ? static_cast<float>(temp_max->valuedouble) : info.temperature_c;
        if (sunrise && cJSON_IsNumber(sunrise)) {
            info.sunrise = std::chrono::system_clock::time_point(std::chrono::seconds(sunrise->valueint));
        } else {
            info.sunrise = {};
        }
        if (sunset && cJSON_IsNumber(sunset)) {
            info.sunset = std::chrono::system_clock::time_point(std::chrono::seconds(sunset->valueint));
        } else {
            info.sunset = {};
        }
        info.description = description && cJSON_IsString(description) ? CapitalizeWords(description->valuestring) : "";
        info.icon = icon && cJSON_IsString(icon) ? icon->valuestring : "";
        info.fetched_at = std::chrono::system_clock::now();
        success = true;
    } while (false);

    cJSON_Delete(root);

    if (!success) {
        ESP_LOGE(TAG, "Weather response missing required fields");
    }
    return success;
}

std::string Application::FormatWeatherSummary(const WeatherInfo& info) const {
    if (info.city.empty() && info.description.empty()) {
        return "";
    }

    int rounded_temp = static_cast<int>(std::round(info.temperature_c));
    char buffer[96];
    if (!info.city.empty() && !info.description.empty()) {
        snprintf(buffer, sizeof(buffer), "%s %d°C %s", info.city.c_str(), rounded_temp, info.description.c_str());
    } else if (!info.city.empty()) {
        snprintf(buffer, sizeof(buffer), "%s %d°C", info.city.c_str(), rounded_temp);
    } else if (!info.description.empty()) {
        snprintf(buffer, sizeof(buffer), "%d°C %s", rounded_temp, info.description.c_str());
    } else {
        snprintf(buffer, sizeof(buffer), "%d°C", rounded_temp);
    }
    return buffer;
}

void Application::UpdateIdleDisplay() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    if (device_state_ != kDeviceStateIdle) {
        display->HideIdleCard();
        return;
    }

    IdleCardInfo card;
    card.greeting = "Hello";

    time_t now = time(nullptr);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) != nullptr && tm_buf.tm_year >= 2025 - 1900) {
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm_buf);
        card.time_text = buffer;
        strftime(buffer, sizeof(buffer), "%A", &tm_buf);
        card.day_text = CapitalizeWords(buffer);
        strftime(buffer, sizeof(buffer), "%m-%d", &tm_buf);
        card.date_text = buffer;
    } else {
        card.time_text = "--:--:--";
    }

    WeatherInfo snapshot;
    bool has_weather = false;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_available_) {
            snapshot = weather_info_;
            has_weather = true;
        }
    }

    auto format_time_hhmm = [](const std::chrono::system_clock::time_point& tp) -> std::string {
        if (tp.time_since_epoch().count() <= 0) {
            return "";
        }
        time_t t = std::chrono::system_clock::to_time_t(tp);
        struct tm tm_buf;
        if (localtime_r(&t, &tm_buf) == nullptr) {
            return "";
        }
        char buffer[16];
        strftime(buffer, sizeof(buffer), "%H:%M", &tm_buf);
        return buffer;
    };

    auto format_wind_dir = [](int deg) -> std::string {
        if (deg < 0) {
            return "";
        }
        static const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        int normalized = ((deg % 360) + 360) % 360;
        int index = (normalized + 22) / 45;
        index = index % 8;
        return dirs[index];
    };

    if (has_weather) {
        card.city = snapshot.city.empty() ? kDefaultWeatherCity : CapitalizeWords(snapshot.city);
        card.temperature_text = std::to_string(static_cast<int>(std::round(snapshot.temperature_c))) + "°C";
        if (snapshot.humidity > 0) {
            card.humidity_text = "Hum " + std::to_string(snapshot.humidity) + "%";
        }
        if (snapshot.feels_like_c != 0.0f) {
            card.feels_like_text = "Feels " + std::to_string(static_cast<int>(std::round(snapshot.feels_like_c))) + "°C";
        }
        if (snapshot.wind_speed > 0.01f) {
            std::string dir = format_wind_dir(snapshot.wind_deg);
            char buffer[48];
            if (!dir.empty()) {
                snprintf(buffer, sizeof(buffer), "Wind %.1f m/s %s", snapshot.wind_speed, dir.c_str());
            } else {
                snprintf(buffer, sizeof(buffer), "Wind %.1f m/s", snapshot.wind_speed);
            }
            card.wind_text = buffer;
        }
        if (snapshot.pressure > 0) {
            card.pressure_text = "Pres " + std::to_string(snapshot.pressure) + " hPa";
        }
        if (!snapshot.description.empty()) {
            card.description_text = snapshot.description;
        } else {
            card.description_text.clear();
        }
        card.uv_text = snapshot.uvi > 0.1f ? "UV " + std::to_string(static_cast<int>(std::round(snapshot.uvi))) : std::string();
        card.sunrise_text = format_time_hhmm(snapshot.sunrise).empty() ? std::string() : "Rise " + format_time_hhmm(snapshot.sunrise);
        card.sunset_text = format_time_hhmm(snapshot.sunset).empty() ? std::string() : "Set " + format_time_hhmm(snapshot.sunset);
        int min_temp = static_cast<int>(std::round(snapshot.temp_min_c));
        int max_temp = static_cast<int>(std::round(snapshot.temp_max_c));
        card.ticker_text = "Lo " + std::to_string(min_temp) + "°C / Hi " + std::to_string(max_temp) + "°C";
        card.icon = WeatherIconFromCode(snapshot.icon);
    } else {
        card.city = kDefaultWeatherCity;
        card.temperature_text.clear();
        card.humidity_text.clear();
        card.feels_like_text.clear();
        card.wind_text.clear();
        card.pressure_text.clear();
        card.uv_text.clear();
        card.sunrise_text.clear();
        card.sunset_text.clear();
        card.ticker_text = Lang::Strings::STANDBY;
        card.description_text = Lang::Strings::STANDBY;
        card.icon = FONT_AWESOME_CLOUD;
    }

    display->ShowIdleCard(card);
}

const char* Application::WeatherIconFromCode(const std::string& code) const {
    if (code.size() < 2) {
        return FONT_AWESOME_CLOUD;
    }
    std::string prefix = code.substr(0, 2);
    if (prefix == "01") {
        return FONT_AWESOME_SUN;
    }
    if (prefix == "02" || prefix == "03") {
        return FONT_AWESOME_CLOUD_SUN;
    }
    if (prefix == "04") {
        return FONT_AWESOME_CLOUD;
    }
    if (prefix == "09" || prefix == "10") {
        return FONT_AWESOME_CLOUD_RAIN;
    }
    if (prefix == "11") {
        return FONT_AWESOME_CLOUD_BOLT;
    }
    if (prefix == "13") {
        return FONT_AWESOME_SNOWFLAKE;
    }
    if (prefix == "50") {
        return FONT_AWESOME_SMOG;
    }
    return FONT_AWESOME_CLOUD;
}
