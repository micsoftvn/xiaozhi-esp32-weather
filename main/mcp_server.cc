/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_err.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>
#include <cctype>
#include <esp_vfs_fat.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"

#define TAG "MCP"

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                auto ota = std::make_unique<Ota>();
                
                bool success = app.UpgradeFirmware(*ota, url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }

    AddTool("self.audio_player.list_tracks",
        "List audio tracks (OGG/Opus) found on the SD card.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& player = Application::GetInstance().GetAudioPlayer();
            auto tracks = player.ScanTracks();
            cJSON* root = cJSON_CreateArray();
            for (const auto& track : tracks) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "title", track.title.c_str());
                cJSON_AddStringToObject(item, "path", track.path.c_str());
                cJSON_AddNumberToObject(item, "size_bytes", static_cast<double>(track.size_bytes));
                cJSON_AddItemToArray(root, item);
            }
            char* json_str = cJSON_PrintUnformatted(root);
            std::string result(json_str ? json_str : "[]");
            if (json_str) {
                cJSON_free(json_str);
            }
            cJSON_Delete(root);
            return result;
        });

    AddTool("self.audio_player.play_track",
        "Play a converted audio track from the SD card.",
        PropertyList({ Property("path", kPropertyTypeString) }),
        [](const PropertyList& properties) -> ReturnValue {
            auto path = properties["path"].value<std::string>();
            bool success = Application::GetInstance().GetAudioPlayer().Play(path);
            return success;
        });

    AddTool("self.audio_player.stop",
        "Stop the current audio playback and hide the player overlay.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().StopAudioPlayback();
            });
            return true;
        });

    AddTool("self.sdcard.get_usage",
        "Get SD card capacity and free space information.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& player = Application::GetInstance().GetAudioPlayer();
            const std::string& mount_point = player.mount_point();
            uint64_t total = 0;
            uint64_t used = 0;
            esp_err_t err = esp_vfs_fat_info(mount_point.c_str(), &total, &used);
            if (err != ESP_OK) {
                throw std::runtime_error("Failed to read filesystem stats: " + std::string(esp_err_to_name(err)));
            }
            uint64_t free = total - used;

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "mount_point", mount_point.c_str());
            cJSON_AddNumberToObject(root, "total_bytes", static_cast<double>(total));
            cJSON_AddNumberToObject(root, "used_bytes", static_cast<double>(used));
            cJSON_AddNumberToObject(root, "free_bytes", static_cast<double>(free));

            char* json_str = cJSON_PrintUnformatted(root);
            std::string result(json_str ? json_str : "{}");
            if (json_str) {
                cJSON_free(json_str);
            }
            cJSON_Delete(root);
            return result;
        });

    AddTool("external.vnexpress.latest",
        "Fetch the latest headlines from VNExpress RSS feed.",
        PropertyList({ Property("limit", kPropertyTypeInteger, 5, 1, 20) }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            int limit = properties["limit"].value<int>();
            auto http = board.GetNetwork()->CreateHttp(5);
            const std::string url = "https://vnexpress.net/rss/tin-moi-nhat.rss";
            if (!http->Open("GET", url)) {
                throw std::runtime_error("Failed to open URL: " + url);
            }
            if (http->GetStatusCode() != 200) {
                throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
            }
            std::string body = http->ReadAll();
            http->Close();

            auto strip_cdata = [](std::string text) {
                const std::string begin = "<![CDATA[";
                const std::string end = "]]>";
                auto start = text.find(begin);
                if (start != std::string::npos) {
                    text = text.substr(start + begin.size());
                    auto finish = text.find(end);
                    if (finish != std::string::npos) {
                        text = text.substr(0, finish);
                    }
                }
                return text;
            };

            auto trim = [](std::string text) {
                auto not_space = [](char ch) { return !std::isspace(static_cast<unsigned char>(ch)); };
                text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
                text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
                return text;
            };

            cJSON* array = cJSON_CreateArray();
            size_t pos = 0;
            while (cJSON_GetArraySize(array) < limit) {
                size_t item_start = body.find("<item>", pos);
                if (item_start == std::string::npos) {
                    break;
                }
                size_t item_end = body.find("</item>", item_start);
                if (item_end == std::string::npos) {
                    break;
                }
                std::string item = body.substr(item_start, item_end - item_start);
                pos = item_end + 7;

                auto extract = [&](const char* tag) -> std::string {
                    std::string open = std::string("<") + tag + ">";
                    std::string close = std::string("</") + tag + ">";
                    size_t start = item.find(open);
                    if (start == std::string::npos) {
                        return "";
                    }
                    start += open.size();
                    size_t end = item.find(close, start);
                    if (end == std::string::npos) {
                        return "";
                    }
                    return item.substr(start, end - start);
                };

                std::string title = strip_cdata(trim(extract("title")));
                std::string link = trim(extract("link"));
                if (title.empty() || link.empty()) {
                    continue;
                }
                cJSON* news = cJSON_CreateObject();
                cJSON_AddStringToObject(news, "title", title.c_str());
                cJSON_AddStringToObject(news, "link", link.c_str());
                cJSON_AddItemToArray(array, news);
            }

            char* json_str = cJSON_PrintUnformatted(array);
            std::string result(json_str ? json_str : "[]");
            if (json_str) {
                cJSON_free(json_str);
            }
            cJSON_Delete(array);
            return result;
        });

    AddTool("external.duckduckgo.search",
        "Search DuckDuckGo for the given query and return quick results.",
        PropertyList({
            Property("query", kPropertyTypeString),
            Property("limit", kPropertyTypeInteger, 5, 1, 10)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto url_encode = [](const std::string& value) -> std::string {
                const char hex[] = "0123456789ABCDEF";
                std::string encoded;
                encoded.reserve(value.size() * 3);
                for (unsigned char c : value) {
                    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        encoded.push_back(static_cast<char>(c));
                    } else if (c == ' ') {
                        encoded.push_back('+');
                    } else {
                        encoded.push_back('%');
                        encoded.push_back(hex[(c >> 4) & 0x0F]);
                        encoded.push_back(hex[c & 0x0F]);
                    }
                }
                return encoded;
            };

            auto query = properties["query"].value<std::string>();
            int limit = properties["limit"].value<int>();

            auto& board = Board::GetInstance();
            auto http = board.GetNetwork()->CreateHttp(5);
            std::string url = "https://api.duckduckgo.com/?q=" + url_encode(query) + "&format=json&no_html=1&skip_disambig=1";
            if (!http->Open("GET", url)) {
                throw std::runtime_error("Failed to open DuckDuckGo API");
            }
            if (http->GetStatusCode() != 200) {
                throw std::runtime_error("DuckDuckGo API returned status " + std::to_string(http->GetStatusCode()));
            }
            std::string body = http->ReadAll();
            http->Close();

            cJSON* root = cJSON_Parse(body.c_str());
            if (!root) {
                throw std::runtime_error("Failed to parse DuckDuckGo response");
            }

            std::vector<std::pair<std::string, std::string>> results;

            auto abstract_text = cJSON_GetObjectItemCaseSensitive(root, "AbstractText");
            auto abstract_url = cJSON_GetObjectItemCaseSensitive(root, "AbstractURL");
            if (cJSON_IsString(abstract_text) && abstract_text->valuestring && *abstract_text->valuestring) {
                std::string text = abstract_text->valuestring;
                std::string url_value = (cJSON_IsString(abstract_url) && abstract_url->valuestring) ? abstract_url->valuestring : "";
                results.emplace_back(std::move(text), std::move(url_value));
            }

            auto append_related = [&](cJSON* node) {
                if (!cJSON_IsObject(node)) {
                    return;
                }
                auto text = cJSON_GetObjectItemCaseSensitive(node, "Text");
                auto first_url = cJSON_GetObjectItemCaseSensitive(node, "FirstURL");
                if (cJSON_IsString(text) && text->valuestring && cJSON_IsString(first_url) && first_url->valuestring) {
                    results.emplace_back(text->valuestring, first_url->valuestring);
                }
            };

            auto related = cJSON_GetObjectItemCaseSensitive(root, "RelatedTopics");
            if (cJSON_IsArray(related)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach(item, related) {
                    if (cJSON_IsObject(item)) {
                        auto topics = cJSON_GetObjectItemCaseSensitive(item, "Topics");
                        if (cJSON_IsArray(topics)) {
                            cJSON* topic = nullptr;
                            cJSON_ArrayForEach(topic, topics) {
                                if (results.size() >= static_cast<size_t>(limit)) {
                                    break;
                                }
                                append_related(topic);
                                if (results.size() >= static_cast<size_t>(limit)) {
                                    break;
                                }
                            }
                        } else {
                            append_related(item);
                        }
                    }
                    if (results.size() >= static_cast<size_t>(limit)) {
                        break;
                    }
                }
            }

            cJSON* arr = cJSON_CreateArray();
            size_t count = std::min(results.size(), static_cast<size_t>(limit));
            for (size_t i = 0; i < count; ++i) {
                cJSON* entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "text", results[i].first.c_str());
                cJSON_AddStringToObject(entry, "url", results[i].second.c_str());
                cJSON_AddItemToArray(arr, entry);
            }

            char* json_str = cJSON_PrintUnformatted(arr);
            std::string result(json_str ? json_str : "[]");
            if (json_str) {
                cJSON_free(json_str);
            }
            cJSON_Delete(arr);
            cJSON_Delete(root);
            return result;
        });

    AddTool("external.vietcombank.usd_rate",
        "Fetch the latest Vietcombank USD exchange rate (buy/transfer/sell).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& board = Board::GetInstance();
            auto http = board.GetNetwork()->CreateHttp(5);
            const std::string url = "https://portal.vietcombank.com.vn/Usercontrols/TVPortal.TyGia/pXML.aspx?b=10";
            http->SetHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64)");
            http->SetHeader("Referer", "https://portal.vietcombank.com.vn/");
            if (!http->Open("GET", url)) {
                throw std::runtime_error("Failed to open Vietcombank exchange rate API");
            }
            if (http->GetStatusCode() != 200) {
                throw std::runtime_error("Vietcombank API returned status " + std::to_string(http->GetStatusCode()));
            }
            std::string body = http->ReadAll();
            http->Close();

            auto find_between = [](const std::string& text, const std::string& begin, const std::string& end) -> std::string {
                size_t start = text.find(begin);
                if (start == std::string::npos) {
                    return "";
                }
                start += begin.size();
                size_t finish = text.find(end, start);
                if (finish == std::string::npos) {
                    return "";
                }
                return text.substr(start, finish - start);
            };

            std::string datetime = find_between(body, "<DateTime>", "</DateTime>");

            size_t usd_pos = body.find("CurrencyCode=\"USD\"");
            if (usd_pos == std::string::npos) {
                throw std::runtime_error("USD rate not found in Vietcombank response");
            }
            size_t tag_start = body.rfind('<', usd_pos);
            size_t tag_end = body.find("/>", usd_pos);
            if (tag_start == std::string::npos || tag_end == std::string::npos) {
                throw std::runtime_error("Unable to parse USD rate entry");
            }
            std::string tag = body.substr(tag_start, tag_end - tag_start);

            auto get_attr = [&](const std::string& name) -> std::string {
                std::string key = name + "=\"";
                size_t start = tag.find(key);
                if (start == std::string::npos) {
                    return "";
                }
                start += key.size();
                size_t finish = tag.find('"', start);
                if (finish == std::string::npos) {
                    return "";
                }
                return tag.substr(start, finish - start);
            };

            std::string buy = get_attr("Buy");
            std::string transfer = get_attr("Transfer");
            std::string sell = get_attr("Sell");
            std::string name = get_attr("CurrencyName");

            if (buy.empty() && transfer.empty() && sell.empty()) {
                throw std::runtime_error("Missing USD exchange values in response");
            }

            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "currency_code", "USD");
            if (!name.empty()) {
                cJSON_AddStringToObject(root, "currency_name", name.c_str());
            }
            if (!datetime.empty()) {
                cJSON_AddStringToObject(root, "timestamp", datetime.c_str());
            }
            if (!buy.empty()) {
                cJSON_AddStringToObject(root, "buy", buy.c_str());
            }
            if (!transfer.empty()) {
                cJSON_AddStringToObject(root, "transfer", transfer.c_str());
            }
            if (!sell.empty()) {
                cJSON_AddStringToObject(root, "sell", sell.c_str());
            }

            char* json_str = cJSON_PrintUnformatted(root);
            std::string result(json_str ? json_str : "{}");
            if (json_str) {
                cJSON_free(json_str);
            }
            cJSON_Delete(root);
            return result;
        });
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
