//屏幕驱动库
#include <TFT_eSPI.h>
//spi驱动库
#include <SPI.h>
//图片解码库
#include <TJpg_Decoder.h>
//json库,用于网页解析
#include "ArduinoJson.h"
//时间库,用于时钟部分
#include <TimeLib.h>
//wifi库
#include <WiFi.h>
//http请求库
#include <HTTPClient.h>
//创建wifi的udp服务的库
#include <WiFiUdp.h>
//静态存储区,用于存储一些固定参数
#include <EEPROM.h>
#include <Arduino.h>
#include "lvgl.h"
//天气图标
#include "weathernum.h"

//蝴蝶GIF
#include "AW4.h"
#include <AnimatedGIF.h>

//温湿度图标
#include "img/temperature.h"
#include "img/humidity.h"

#include "img/wx.h"


//字体文件用于时钟显示
#include "font/sj38.h"
//字体文件用于星期和日月,粉丝数显示
#include "font/zkyyt12.h"
//字体文件用于城市名字显示
#include "font/ALBB17.h"
//字体文件用于天气显示
#include "font/tq17.h"

#include "font/ALBB10.h"

#include "font/ALBB18.h"



//--------------手机热点信息
const char *ssid = "micsoftvn"; 
const char *password = "123@1234"; 
//--------------------------

//在""内修改字体内容
String follower = "Hello";

//CityCode文件夹内的js文件搜索可得 城市ID 
//--------------------

WeatherNum wrat;  //天气对象
int prevTime = 0;
int AprevTime = 0;
int Anim = 0;
//背景颜色
uint16_t bgColor = 0x0000;

//字体颜色

//时间小时分钟字体颜色
uint16_t timehmfontColor = 0xFFFF;
//时间秒字体颜色
uint16_t timesfontColor = 0xFFFF;
//星期字体颜色
uint16_t weekfontColor = 0xFFFF;
//日月字体颜色
uint16_t monthfontColor = 0xFFFF;
//温湿度字体颜色
uint16_t thfontColor = 0xFFFF;
//左上角轮播字体颜色
uint16_t tipfontColor = 0xFFFF;
//城市名称字体颜色
uint16_t cityfontColor = 0xFFFF;
//空气质量字体颜色
uint16_t airfontColor = 0xFFFF;
//红色
uint16_t fontColor = 0xFF00;
uint16_t bilifontColor = 0xF81F;
//线框颜色
uint16_t xkColor = 0xFFFF;

//=================

AnimatedGIF gif;
#define DISPLAY_WIDTH  tft.width()
#define DISPLAY_HEIGHT tft.height()
#define BUFFER_SIZE 256            // Optimum is >= GIF width or integral division 

uint16_t usTemp[1][BUFFER_SIZE];    // Global to support DMA use

bool     dmaBuf = 0;

//=================


//时钟部分参数---------------------------------
//NTP服务器
static const char ntpServerName[] = "time.nist.gov";//ntp6.aliyun.com
float timeZone;

#define ciry_i 1 //1==>自动获取所在城市ID 0==>手动设置
static String cityCode = "Lisbon"; //在CityCode文件夹的citycode.json文件 搜自己所在的城市名称，将其复制到" "此处
char* api_key = "fbf5a0e942e6fea3ff18103b9fd46ed9";


WiFiUDP Udp;
unsigned int localPort = 8000;
WiFiClient wificlient; 

time_t getNtpTime();
void digitalClockDisplay();
void printDigits(int digits);
String num2str(int digits);
void sendNTPpacket(IPAddress &address);

byte setNTPSyncTime = 20;  //设置NTP时间同步频率，10分钟同步一次
int backLight_hour = 0;
time_t prevDisplay = 0;  // 显示时间

//----------------------温湿度参数------------------
unsigned long wdsdTime = 0;
byte wdsdValue = 0;
String wendu1 = "",wendu2 = "",shidu = "",yaqiang = "",tianqi = "",kjd = "";

//----------------------------------------------------

//------------------LVGL 太空人参数------------------

static const uint16_t screenWidth = 240;
static const uint16_t screenHeight = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenHeight * screenWidth / 10];
TFT_eSPI tft = TFT_eSPI(240,240);  // 引脚请自行配置tft_espi库中的 User_Setup.h文件
TFT_eSprite clk = TFT_eSprite(&tft);

LV_IMG_DECLARE(TKR_A);
static lv_obj_t *logo_imga = NULL;

//==========================
void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette;
  int x, y, iWidth, iCount;

  // pDraw->iX = 50;
  // pDraw->iY = 50;

  // Displ;ay bounds chech and cropping
  iWidth = pDraw->iWidth;
  if (iWidth + pDraw->iX > DISPLAY_WIDTH)
    iWidth = DISPLAY_WIDTH - pDraw->iX;
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y; // current line
  if (y >= DISPLAY_HEIGHT || pDraw->iX >= DISPLAY_WIDTH || iWidth < 1)
    return;

  // Old image disposal
  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < iWidth; x++)
    {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) // if transparency used
  {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0; // count non-transparent pixels
    while (x < iWidth)
    {
      c = ucTransparent - 1;
      d = &usTemp[0][0];
      while (c != ucTransparent && s < pEnd && iCount < BUFFER_SIZE )
      {
        c = *s++;
        if (c == ucTransparent) // done, stop
        {
          s--; // back up to treat it like transparent
        }
        else // opaque
        {
          *d++ = usPalette[c];
          iCount++;
        }
      } // while looking for opaque pixels
      if (iCount) // any opaque pixels?
      {
        // DMA would degrtade performance here due to short line segments
        tft.setAddrWindow(pDraw->iX + x, y, iCount, 1);
        tft.pushPixels(usTemp, iCount);
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent)
          x++;
        else
          s--;
      }
    }
  }
  else
  {
    s = pDraw->pPixels;

    // Unroll the first pass to boost DMA performance
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    if (iWidth <= BUFFER_SIZE)
      for (iCount = 0; iCount < iWidth; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];
    else
      for (iCount = 0; iCount < BUFFER_SIZE; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];

#ifdef USE_DMA // 71.6 fps (ST7796 84.5 fps)
    tft.dmaWait();
    tft.setAddrWindow(pDraw->iX, y, iWidth, 1);
    tft.pushPixelsDMA(&usTemp[dmaBuf][0], iCount);
    dmaBuf = !dmaBuf;
#else // 57.0 fps
    tft.setAddrWindow(pDraw->iX, y, iWidth, 1);
    tft.pushPixels(&usTemp[0][0], iCount);
#endif

    iWidth -= iCount;
    // Loop if pixel buffer smaller than width
    while (iWidth > 0)
    {
      // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
      if (iWidth <= BUFFER_SIZE)
        for (iCount = 0; iCount < iWidth; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];
      else
        for (iCount = 0; iCount < BUFFER_SIZE; iCount++) usTemp[dmaBuf][iCount] = usPalette[*s++];

#ifdef USE_DMA
      tft.dmaWait();
      tft.pushPixelsDMA(&usTemp[dmaBuf][0], iCount);
      dmaBuf = !dmaBuf;
#else
      tft.pushPixels(&usTemp[0][0], iCount);
#endif
      iWidth -= iCount;
    }
  }
} /* GIFDraw() */

//==========================


//------------------------------------------------------------
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {

  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  // Return 1 to decode next block
  return 1;
}

//lvgl 回调函数
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void tkr()
{
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenHeight * screenWidth / 10);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  /*Change the following line to your display resolution*/
  disp_drv.hor_res = 240;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;  
  lv_disp_drv_register(&disp_drv);

  static lv_style_t style;  
  lv_style_init(&style);
  lv_style_set_bg_color(&style, lv_color_black());
  lv_obj_add_style(lv_scr_act(), &style, 0);

  logo_imga = lv_gif_create(lv_scr_act());
  lv_obj_center(logo_imga);
  lv_obj_align(logo_imga, LV_ALIGN_LEFT_MID, 80, 0);
  lv_gif_set_src(logo_imga, &TKR_A);

  lv_timer_handler();
  delay(1);
}


//-----------------------------------城市代码获取及展示
//获取城市信息ID
void getCityCode() {
String URL = "http://ip-api.com/json/?fields=city,lat,lon";

HTTPClient httpClient;
httpClient.begin(wificlient,URL);

//启动连接并发送HTTP请求
  int httpCode = httpClient.GET();
  Serial.print("Send GET request to URL: ");
  Serial.println(URL);

  if (httpCode == HTTP_CODE_OK) {
    
    DynamicJsonDocument doc(1024);
    deserializeJson(doc,httpClient.getString());
    String city = doc["city"];
    if(ciry_i==1)
      cityCode=city; //获取当地城市ID
  }
  httpClient.end();
}

void getCityTime(){
  getCityCode();
  String URL = "http://api.openweathermap.org/data/2.5/weather?q=" + cityCode + "&appid=" + String(api_key) + "&units=metric"; //524901
  HTTPClient httpClient;
  httpClient.begin(URL);

  int httpCode = httpClient.GET();
   if (httpCode == HTTP_CODE_OK) {
      const size_t capacity = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(5) + JSON_OBJECT_SIZE(6) + JSON_OBJECT_SIZE(13) + JSON_OBJECT_SIZE(40) + 470;
      DynamicJsonDocument doc(capacity);
      deserializeJson(doc,httpClient.getString());

      long timezone1 = doc["timezone"]; //获取时区信息
      timeZone=(float)(timezone1/3600);

      Serial.println("获取城市信息成功");
   }else {
    Serial.println("获取城市信息失败");
    Serial.print(httpCode);
  }
  httpClient.end();
}

int temp_i1,temp_i2;
String scrollText[5];

// 获取城市天气
void getCityWeater(){

  float temp_f,temp_min_f,temp_max_f;
  getCityCode();
  String URL = "http://api.openweathermap.org/data/2.5/weather?q=" + cityCode + "&appid=" + String(api_key) + "&units=metric"; //524901
 
  //创建 HTTPClient 对象
 HTTPClient httpClient;
 httpClient.begin(URL);


  int httpCode = httpClient.GET();
  Serial.println("正在获取天气数据");
  Serial.println(URL);

  //如果服务器响应OK则从服务器获取响应体信息并通过串口输出
  if (httpCode == HTTP_CODE_OK) {

    const size_t capacity = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(5) + JSON_OBJECT_SIZE(6) + JSON_OBJECT_SIZE(13) + JSON_OBJECT_SIZE(40) + 470;
    DynamicJsonDocument doc(capacity);
    deserializeJson(doc,httpClient.getString());
    // JsonObject sk = doc.as<JsonObject>();
    // String str1 = httpClient.getString();
 
    float temp = doc["main"]["temp"]; // Temperature in Celsius
    int humidity = doc["main"]["humidity"]; // Humidity in percentage
    int pressure = doc["main"]["pressure"];
    String description = doc["weather"][0]["description"]; // Weather description 天气
    String icon = doc["weather"][0]["icon"];
    float temp_min = doc["main"]["temp_min"];
    float temp_max = doc["main"]["temp_max"];
    int visibility = doc["visibility"];
    
    temp_f = 32 + temp*1.8;
    temp_min_f = 32 + temp_min*1.8;
    temp_max_f = 32 + temp_max*1.8;

    temp_i1 = int(temp_f);
    temp_i2 = int(temp);

    wendu1 = String(temp_i1);//温度 ℉
    wendu2 = String(temp_i2); //温度 ℃
    shidu = String(humidity); //湿度
    yaqiang = String(pressure); //压强
    tianqi = String(description);//天气
    kjd = String(visibility/1000); //可见度
    
    clk.setColorDepth(8);
    clk.loadFont(tq17);
  //城市名称
  clk.createSprite(140, 18);
  clk.fillSprite(bgColor);
  clk.setTextDatum(ML_DATUM);
  clk.setTextColor(cityfontColor, bgColor);
  clk.drawString(cityCode,1,8); //
  clk.pushSprite(1,5);
  clk.deleteSprite();
  clk.unloadFont();

  scrollText[0] = "MIN T "+String(temp_min_f)+ " F / "+String(temp_min)+"℃";
  scrollText[1] = "MAX T "+String(temp_max_f)+ " F / "+String(temp_max)+"℃";
  scrollText[2] = "Weather "+String(tianqi);
  scrollText[3] = "pressure "+String(yaqiang)+"hPa";
  scrollText[4] = "visibility "+String(kjd)+"km";
  wrat.printfweather1(1,130,icon);

  Serial.println("获取成功");

  } else {
    Serial.println("请求城市天气错误：");
    Serial.print(httpCode);
  }

  //关闭ESP32与服务器连接
  httpClient.end();
}


//---------------- 温湿度轮播显示------------------------------------------

void weatherWarning() {  //间隔5秒切换显示温度和湿度，该数据为气象站获取的室外参数
  if (millis() - wdsdTime > 5000) {
    wdsdValue = wdsdValue + 1;
    //Serial.println("wdsdValue0" + String(wdsdValue));
    clk.setColorDepth(8);
    clk.loadFont(tq17);
    switch (wdsdValue) {
      case 1:
        //Serial.println("wdsdValue1" + String(wdsdValue));
        TJpgDec.drawJpg(165, 150, temperature, sizeof(temperature));  //温度图标
        for (int i = 20; i > 0; i--) {
          clk.createSprite(70, 16);
          clk.fillSprite(bgColor);
          clk.setTextDatum(ML_DATUM);
          clk.setTextColor(thfontColor, bgColor);
          clk.drawString(wendu1 + " F", 1, i + 8);
          clk.pushSprite(185, 150);
          clk.deleteSprite();
          delay(10);
        }
        break;
      case 2:
        //Serial.println("wdsdValue2" + String(wdsdValue));
        TJpgDec.drawJpg(165, 150, temperature, sizeof(temperature));  //温度图标
        for (int i = 20; i > 0; i--) {
          clk.createSprite(70, 16);
          clk.fillSprite(bgColor);
          clk.setTextDatum(ML_DATUM);
          clk.setTextColor(thfontColor, bgColor);
          clk.drawString(wendu2+"℃", 1, i + 8);
          clk.pushSprite(185, 150);
          clk.deleteSprite();
          delay(10);
        }
        break;
        case 3:
      //Serial.println("wdsdValue2" + String(wdsdValue));
          TJpgDec.drawJpg(165,150,humidity, sizeof(humidity));  //湿度图标
          for(int i=20;i>0;i--) {
          clk.createSprite(70, 16);
          clk.fillSprite(bgColor);
          clk.setTextDatum(ML_DATUM);
          clk.setTextColor(thfontColor, bgColor);
          clk.drawString(shidu+"%",1,i+8);
          clk.pushSprite(185, 150);
          clk.deleteSprite();
          delay(10);
        }
        wdsdValue = 0;
        break;
    }
    wdsdTime = millis();
    clk.unloadFont();
  }
}

//----------------------------天气信息轮播-------------------------
int currentIndex = 0;

TFT_eSprite clkb = TFT_eSprite(&tft);

void scrollBanner() {
  if (millis() - prevTime > 3500) {  //3.5秒切换一次

    if (scrollText[currentIndex]) {

      clkb.setColorDepth(8);
      clkb.loadFont(tq17);

      for (int pos = 20; pos > 0; pos--) {
        scrollTxt(pos);
      }

      clkb.deleteSprite();
      clkb.unloadFont();

      if (currentIndex >= 4) {
        currentIndex = 0;  //回第一个
      } else {
        currentIndex += 1;  //准备切换到下一个
      }
    }
    prevTime = millis();
  }
}

void scrollTxt(int pos) {
  clkb.createSprite(240, 16);
  clkb.fillSprite(bgColor);
  clkb.setTextWrap(false);
  clkb.setTextDatum(ML_DATUM);
  clkb.setTextColor(tipfontColor, bgColor);
  clkb.drawString(scrollText[currentIndex], 1, pos + 8);
  clkb.pushSprite(35, 215);
}

//----------------------------------------------

void get_wifi() {
  // 连接网络
  WiFi.begin(ssid, password);
  //等待wifi连接
  while (WiFi.status() != WL_CONNECTED) {
    delay(10);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");  //连接成功
  Serial.print("IP address: ");      //打印IP地址
  Serial.println(WiFi.localIP());
}

void fanspush() { //显示Hello 字符串
  TJpgDec.drawJpg(1, 215, wx, sizeof(wx));
  clk.createSprite(60, 18);
  clk.fillSprite(bgColor);
  clk.loadFont(ALBB18);
  clk.setTextColor(fontColor, bgColor);
  clk.drawString(String(follower), 1, 9);
  clk.pushSprite(170, 3);
  clk.deleteSprite();
  clk.unloadFont();
}


void setup() {
  
  tkr();
  Serial.begin(115200);
  tft.begin();           /* TFT init */ 
  tft.invertDisplay(1);  //反转所有显示颜色：1反转，0正常
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, 0x0000);

  tft.setRotation(2);
  gif.begin(BIG_ENDIAN_PIXELS);
  if (gif.open((uint8_t *)AW4, sizeof(AW4), GIFDraw))
  {
      Serial.printf("Successfully opened GIF; Canvas size = %d x %d\n", gif.getCanvasWidth(), gif.getCanvasHeight());
      tft.startWrite(); // The TFT chip slect is locked low
      while (gif.playFrame(true, NULL))
      {
        yield();
      }
      gif.close();
      tft.endWrite(); // Release TFT chip select for other SPI devices
  }
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0,8,2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // 设置显示的文字，注意这里有个换行符 \n 产生的效果
  tft.println("---------------------------------------");
  tft.println("Configure mobile WLAN information:");
  tft.println("WIFI name:micsoftvn");
  tft.println("WIFI password: 123@1234\n---------------------------------------");
  tft.println("If there is a city information error,execute==>After turning on the phone WLAN,turn off the phone data,and then connect to the router WIFI.");
  tft.println("---------------------------------------");

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);

  get_wifi();

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);


  setSyncProvider(getNtpTime);
  setSyncInterval(setNTPSyncTime * 60);  //NTP网络同步频率，单位秒。



  tft.fillScreen(TFT_BLACK);
  tft.fillRoundRect(0, 0, 128, 128, 0, bgColor);  //实心矩形


  //绘制线框
  tft.drawFastHLine(0, 0, 240, xkColor);


  tft.drawFastHLine(0, 25, 240, xkColor);
  // tft.drawFastHLine(0, 120, 240, xkColor);

  tft.drawFastVLine(150, 0, 25, xkColor);

  tft.drawFastHLine(0, 170, 240, xkColor);

  tft.drawFastVLine(150, 170, 36, xkColor);
  // tft.drawFastVLine(60, 170, 36, xkColor);
  tft.drawFastHLine(0,205,240,xkColor);
  int TCityCODE = 0;

  if (TCityCODE >= 101000000 && TCityCODE <= 102000000)
    cityCode = String(TCityCODE);
  else{
    getCityCode();  //获取城市代码 101020200 101280601
    // delay(10);
    }
  getCityWeater();
  fanspush();
}



unsigned long weaterTime = 0;
void loop() {
  
  if (now() != prevDisplay) {
    prevDisplay = now();
    digitalClockDisplay();
  }

  if (millis() - weaterTime > 300000) { //1分钟更新一次天气
    weaterTime = millis(); 
    getCityWeater();
  }
  
  digitalClockDisplay();
  scrollBanner();
  weatherWarning();
  lv_timer_handler();
  delay(1);
}




//时钟显示函数--------------------------------------------------------------------------

//利用分辨率缩放  *(128/240)

void digitalClockDisplay() {

  clk.setColorDepth(8);

  /***中间时间区***/
  //时分
  clk.createSprite(100, 50);
  clk.fillSprite(bgColor);
  clk.loadFont(sj38);
  clk.setTextDatum(ML_DATUM);
  clk.setTextColor(timehmfontColor, bgColor);
  clk.drawString(hourMinute(), 1, 16, 7);  //绘制时和分
  clk.unloadFont();
  clk.pushSprite(50, 45);
  clk.deleteSprite();

  //秒
  clk.createSprite(50, 50);
  clk.fillSprite(bgColor);

  clk.loadFont(sj38);
  clk.setTextDatum(ML_DATUM);
  clk.setTextColor(timesfontColor, bgColor);
  clk.drawString(num2str(second()), 1, 16);

  clk.unloadFont();
  clk.pushSprite(150, 45);
  clk.deleteSprite();
  /***中间时间区***/

  /***底部***/
  clk.loadFont(ALBB18);
  clk.createSprite(120, 25);
  clk.fillSprite(bgColor);

  //星期
  clk.setTextDatum(ML_DATUM);
  clk.setTextColor(weekfontColor, bgColor);
  clk.drawString(week(), 1, 10);
  clk.pushSprite(1, 180);
  clk.deleteSprite();

  //月日
  clk.createSprite(78, 25);
  clk.fillSprite(bgColor);
  clk.setTextDatum(ML_DATUM);
  clk.setTextColor(monthfontColor, bgColor);
  clk.drawString(monthDay(), 1, 10);
  clk.pushSprite(165, 180);//位置
  clk.deleteSprite();

  clk.unloadFont();
  /***底部***/
}

//星期
String week() {
  String wk[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  String s = wk[weekday() - 1];
  return s;
}

//月日
String monthDay() {
  String s = String(month());
  s = s + "-" + day();
  return s;
}
//时分
String hourMinute() {
  String s = num2str(hour());
  backLight_hour = s.toInt();
  s = s + ": " + num2str(minute());
  return s;
}

String num2str(int digits) {
  String s = "";
  delay(9);//调整时间的快慢
  if (digits < 10)
    s = s + "0";
  s = s + digits;
  return s;
}

void printDigits(int digits) {
  Serial.print(":");
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}
//------------------------------------------------------------------------------------


//NTP部分的代码,包含2个函数------------------------------------------------------------

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48;      // NTP时间在消息的前48字节中
byte packetBuffer[NTP_PACKET_SIZE];  //buffer to hold incoming & outgoing packets

time_t getNtpTime() {
  getCityTime();
  IPAddress ntpServerIP;  // NTP server's ip address

  while (Udp.parsePacket() > 0)
    ;  // discard any previously received packets
  //Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  //Serial.print(ntpServerName);
  //Serial.print(": ");
  //Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("NTP同步成功");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 = (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      //Serial.println(secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR);
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  //ESP.restart(); //时间获取失败直接重启
  Serial.println("NTP同步失败");
  return 0;  // 无法获取时间时返回0
}

// 向NTP服务器发送请求
void sendNTPpacket(IPAddress &address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;  // LI, Version, Mode
  packetBuffer[1] = 0;           // Stratum, or type of clock
  packetBuffer[2] = 6;           // Polling Interval
  packetBuffer[3] = 0xEC;        // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123);  //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

//--------------------------------------------------------------------------
