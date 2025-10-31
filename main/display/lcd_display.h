#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <array>
#include <atomic>
#include <memory>
#include <chrono>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    lv_obj_t* audio_panel_ = nullptr;
    lv_obj_t* audio_title_label_ = nullptr;
    std::array<lv_obj_t*, 8> audio_bars_{};
    lv_obj_t* audio_stop_button_ = nullptr;
    std::chrono::steady_clock::time_point last_spectrum_update_{};

    lv_obj_t* idle_panel_ = nullptr;
    lv_obj_t* idle_city_label_ = nullptr;
    lv_obj_t* idle_greeting_label_ = nullptr;
    lv_obj_t* idle_time_label_ = nullptr;
    lv_obj_t* idle_icon_label_ = nullptr;
    lv_obj_t* idle_temp_label_ = nullptr;
    lv_obj_t* idle_humidity_label_ = nullptr;
    lv_obj_t* idle_day_label_ = nullptr;
    lv_obj_t* idle_date_label_ = nullptr;
    lv_obj_t* idle_desc_label_ = nullptr;
    bool idle_mode_enabled_ = false;

    void InitializeLcdThemes();
    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void HideIdleCardInternal();

protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void ShowAudioPlayer(const std::string& title) override;
    virtual void UpdateAudioSpectrum(const std::array<uint8_t, 8>& bars) override;
    virtual void HideAudioPlayer() override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    virtual void ShowIdleCard(const IdleCardInfo& info) override;
    virtual void UpdateIdleCardTime(const std::string& time_text) override;
    virtual void HideIdleCard() override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
};

// SPI LCD显示器
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD显示器
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD显示器
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
