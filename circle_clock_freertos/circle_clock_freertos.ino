/* ****************************************************************************************
 * 
 * circle clock (FreeRTOS-Version)
 * ===============================
 *       Uwe Berger; 2026
 * 
 * 
 * Hardware:
 * ---------
 *   --> https://www.waveshare.com/esp32-s3-lcd-1.28.htm
 *   --> https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28
 *
 *  
 * Funktionen:
 * ----------- 
 * - WiFi-Manager
 * - NTP-Client
 * - Kreisuhr
 * - Cheat-Mode
 * 
 * Uhr (Kreise von aussen nach innen):
 * --------------------------------------
 * * Sekunden
 * * Minuten
 * * Stunden (24h)
 * * Sonnenauf-/-untergang
 * * Tag in der Woche
 * * Tag im Monat
 * * Tag im Jahr
 * * NTP-Sync-Status
 * 
 *
 * =========
 * Have fun! 
 * 
 * ****************************************************************************************
*/

#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Arduino_GFX_Library.h>
#include <QMI8658.h>
#include <time.h>
#include "esp_sntp.h"
#include "astro.h"

// BOOT-Button
#define BOOT_BTN    0   // GPIO Boot-Button
volatile bool boot_btn_down = false;
volatile uint32_t boot_btn_start = 0;

// GPIOs display am ESP
#define TFT_CS 9
#define TFT_DC 8
#define TFT_RST 12
#define TFT_SCK 10
#define TFT_MOSI 11
#define TFT_MISO -1  // es kommen keine Daten zurück
#define TFT_BL 40

#if defined(DISPLAY_DEV_KIT)
Arduino_GFX *gfx = create_default_Arduino_GFX();
#else /* !defined(DISPLAY_DEV_KIT) */
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GC9A01 *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);
#endif /* !defined(DISPLAY_DEV_KIT) */

// QMI8658
// ...GPIOS so default, reine Doku!
#define IMU_SDA  6
#define IMU_SCL  7
#define IMU_IRQ  -1
// ...
QMI8658 imu;

// Kreise...
// ...Farbdefinitionen
#define BLACK       0x0000
#define WHITE       0xFFFF
#define MIDDLE_GREY 0x8410
#define RED         0xF800
#define YELLOW      0xFFE0
#define GREEN       0x07E0
#define DARKGREEN   0x03E0
#define BLUE        0x001F
// ...(RGB565)
#define ORANGE      0xFD20   // RGB(255,165,0)
#define NAVY        0x000F   // RGB(0,0,128)
#define PURPLE      0x780F   // RGB(128,0,128)
#define MAROON      0x7800   // RGB(128,0,0)
#define DARKGREY    0x7BEF   // RGB(123,123,123)
#define LIGHTGREY   0xC618   // RGB(198,198,198)
// ...Elemente
#define BACKGROUND              BLACK
#define COLOR_SECOND            GREEN
#define COLOR_MINUTE            BLUE
#define COLOR_HOUR              RED
#define COLOR_CLEAR             BLACK
#define COLOR_SUN               ORANGE
#define COLOR_DAY_OF_WEEK       NAVY
#define COLOR_DAY_OF_MONTH      PURPLE
#define COLOR_MONTH             MAROON
#define COLOR_BATTERY_GOOD      DARKGREEN
#define COLOR_BATTERY_BAD       RED
#define COLOR_TIMESTATUS_GOOD   DARKGREEN
#define COLOR_TIMESTATUS_BAD    RED
#define COLOR_MARK              DARKGREY
#define COLOR_SUBMARK           LIGHTGREY
// ...Kreisradien
#define OFFSET_RO_SECOND        0
#define OFFSET_RI_SECOND        4
#define OFFSET_RO_MINUTE        5
#define OFFSET_RI_MINUTE        24
#define OFFSET_RO_HOUR          24
#define OFFSET_RI_HOUR          43
#define OFFSET_RO_SUN           44
#define OFFSET_RI_SUN           50 //48
#define OFFSET_RO_DAY_OF_WEEK   55
#define OFFSET_RI_DAY_OF_WEEK   69
#define OFFSET_RO_DAY_OF_MONTH  69
#define OFFSET_RI_DAY_OF_MONTH  83
#define OFFSET_RO_MONTH         83
#define OFFSET_RI_MONTH         97
#define OFFSET_RO_BATTERY       105
#define OFFSET_RI_BATTERY       110
#define OFFSET_RO_TIMESTATUS    113
#define OFFSET_RI_TIMESTATUS    117
// Orientierung für grafische Elemente in Grad (Winkel)
#define ORIENTATION_DEG         270.0

// pi
#define PI  3.1415926536

// lokale Zeitzone
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03" 

// miscellaneous
static int16_t w, h, center;

volatile bool time_is_sync = false;
volatile long time_last_sync = 0;
volatile bool cheat_mode = false;

// WiFi-Manager
WiFiManager wm;

// FreeRTOS Mutexe...
SemaphoreHandle_t mutex_display = NULL;

// FreeRTOS Handles
TaskHandle_t handle_task_display_second_circle;
TaskHandle_t handle_task_display_minute_circle;
TaskHandle_t handle_task_display_hour_circle;
TaskHandle_t handle_task_display_sun_circle;
TaskHandle_t handle_task_display_day_of_week_circle;
TaskHandle_t handle_task_display_day_of_month_circle;
TaskHandle_t handle_task_display_month_circle;
TaskHandle_t handle_task_display_time_is_sync_circle;
TaskHandle_t handle_task_display_cheat_mode;

// *********************************************************************
void IRAM_ATTR boot_btn_isr()
{
    if (digitalRead(BOOT_BTN) == LOW) {
        boot_btn_down = true;
        boot_btn_start = millis();
    } else {
        boot_btn_down = false;
        boot_btn_start = 0;
    }
}

// *********************************************************************
// Callback bei erfolgreichem NTP-Sync
void time_sync_cb(struct timeval *tv)
{
    time_last_sync = tv->tv_sec;
    time_is_sync = true;
}

// *********************************************************************
int days_in_month(int year, int mon) 
{
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    // Februar + Schaltjahr
    if (mon == 1) {   // 0 = Januar, 1 = Februar, ...
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return mdays[mon];
}

// *********************************************************************
void display_setup_message(String msg)
{
	static int y = 50;
	const int x = 40;
	const int dy = 15;
    gfx->setTextSize(1);
    gfx->setCursor(x, y);
    gfx->println(msg);
    y = y + dy;		
}

// *********************************************************************
void draw_round_clock_mark(int16_t inner, int16_t outer, int16_t count, int16_t mark)
{
    float angle = 2 * PI / count;
    float startOffset = ORIENTATION_DEG * PI / 180.0;   // 270° als Grad-Define
    for (int i = 0; i < count; i++) {
        uint16_t color = (i % mark == 0) ? COLOR_SUBMARK : COLOR_MARK;
        float rad = i * angle + startOffset;
        int16_t x1 = cos(rad) * inner + center;
        int16_t y1 = sin(rad) * inner + center;
        int16_t x2 = cos(rad) * outer + center;
        int16_t y2 = sin(rad) * outer + center;
        gfx->drawLine(x1, y1, x2, y2, color);
    }
    gfx->drawCircle(center, center, inner, COLOR_MARK);
    gfx->drawCircle(center, center, outer, COLOR_MARK);
}

// *********************************************************************
void task_boot_button(void *parameter)
{
    // Task-Loop
    while(1) {
        if (boot_btn_down && (millis() - boot_btn_start >= 3000)) {
            // nach 3s BOOT-Taste gedrückt -> WiFi-Infos löschen & reboot
            wm.resetSettings();
            delay(500);
            ESP.restart();
            delay(500);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);   // 500 ms Task-Sleep
    }
}

// *********************************************************************
void task_display_second_circle(void *parameter)
{
    time_t now;
    tm tm;
    struct timeval tv;
    int ms;
    float oldSecond_deg = 0;
    float currentSecond_deg;
    int oldRotation = -1;
    int rotation;
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        gettimeofday(&tv, NULL);    // Unixtime (Millisekunden werden gebraucht)
        now = tv.tv_sec;
        localtime_r(&now, &tm);
        ms = tv.tv_usec / 1000;     // Millisekunden
        // Sekundenkreis zeichnen
        currentSecond_deg = 360.0/60*tm.tm_sec + 360.0/60*ms/1000;
        rotation = gfx->getRotation();
        if ((currentSecond_deg < oldSecond_deg) || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_SECOND, center - OFFSET_RI_SECOND, 0.0, 360.0, COLOR_CLEAR);
                xSemaphoreGive(mutex_display);
            }
            oldRotation = rotation;
            oldSecond_deg = 0.0;
        }
        if ((currentSecond_deg-oldSecond_deg) > 0.5) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_SECOND, center - OFFSET_RI_SECOND, oldSecond_deg + ORIENTATION_DEG, currentSecond_deg + ORIENTATION_DEG, COLOR_SECOND);
                xSemaphoreGive(mutex_display);
            }
            oldSecond_deg = currentSecond_deg;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
void task_display_minute_circle(void *parameter)
{
    time_t now;
    tm tm;
    float oldMinute_deg = 0;
    float currentMinute_deg;
    int oldRotation = -1;
    int rotation;    
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);
        // Minutenkreis zeichnen
        currentMinute_deg = 360.0/60*tm.tm_min + 360.0/60*tm.tm_sec/60;
        rotation = gfx->getRotation();        
        if ((currentMinute_deg < oldMinute_deg) || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, 0.0, 360.0, COLOR_CLEAR);
                draw_round_clock_mark(center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, 60, 5);
                xSemaphoreGive(mutex_display);
            }
            oldRotation = rotation;
            oldMinute_deg = 0.0;
        }
        if ((currentMinute_deg > oldMinute_deg)  && ((currentMinute_deg-oldMinute_deg) > 1.0)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, oldMinute_deg + ORIENTATION_DEG, currentMinute_deg + ORIENTATION_DEG, COLOR_MINUTE);
                draw_round_clock_mark(center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, 60, 5);
                xSemaphoreGive(mutex_display);
            }
            oldMinute_deg = currentMinute_deg;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
void task_display_hour_circle(void *parameter)
{
    time_t now;
    tm tm;
    float oldHour_deg = 0;
    float currentHour_deg;
    int oldRotation = -1;
    int rotation; 
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);    
        // Stundenkreis zeichnen
        currentHour_deg = 360.0/24*tm.tm_hour + 360.0/24*tm.tm_min/60;
        rotation = gfx->getRotation();         
        if ((currentHour_deg < oldHour_deg) || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, 0.0, 360.0, COLOR_CLEAR);
                draw_round_clock_mark(center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, 24, 3);
                xSemaphoreGive(mutex_display);
            }
            oldRotation = rotation;
            oldHour_deg = 0.0;
        }
        if ((currentHour_deg > oldHour_deg) && ((currentHour_deg-oldHour_deg) > 1.0)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, oldHour_deg + ORIENTATION_DEG, currentHour_deg + ORIENTATION_DEG, COLOR_HOUR);
                draw_round_clock_mark(center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, 24, 3);
                xSemaphoreGive(mutex_display);
            }
            oldHour_deg = currentHour_deg;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
void task_display_sun_circle(void *parameter)
{
    time_t now;
    tm tm;
    uint16_t oldDayOfYear = 0;
    uint16_t currentDayOfYear;
    boolean currentIsDST;
    int oldRotation = -1;
    int rotation;     
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);   
        // Sonnenauf-/-untergangskreis zeichnen
        currentDayOfYear = tm.tm_yday;              // <<== +1? (aber auf dem Display kann man es sowieso nicht so genau erkennen/differenzieren :-)...) 
        //~ Serial.println(currentDayOfYear);
        currentIsDST = tm.tm_isdst;                 // <<==?? (scheint zu passen)
        rotation = gfx->getRotation();
        if ((currentDayOfYear != oldDayOfYear)  || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_SUN, center - OFFSET_RI_SUN, 0.0, 360.0, COLOR_CLEAR);
                float currentSunrise = sunrise(currentDayOfYear, currentIsDST);
                int sunriseHour = int(currentSunrise);
                int sunriseMinute = round((currentSunrise - sunriseHour) * 60.0);
                float sunrise_deg = 360.0/24*sunriseHour + 360.0/24*sunriseMinute/60;
                float currentSunset = sunset(currentDayOfYear, currentIsDST);
                int sunsetHour = int(currentSunset);
                int sunsetMinute = round((currentSunset - sunsetHour) * 60.0);   
                float sunset_deg = 360.0/24*sunsetHour + 360.0/24*sunsetMinute/60;
                gfx->fillArc(center, center, center - OFFSET_RO_SUN, center - OFFSET_RI_SUN, sunrise_deg + ORIENTATION_DEG, sunset_deg + ORIENTATION_DEG, COLOR_SUN);
                xSemaphoreGive(mutex_display);
            }
            oldDayOfYear = currentDayOfYear;
            oldRotation = rotation;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);    
    }
}

// *********************************************************************
void task_display_day_of_week_circle(void *parameter)
{
    time_t now;
    tm tm;
    int day_of_week;
    int old_day_of_week = -1;
    int oldRotation = -1;
    int rotation;     
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);
        // Wochentagskreis zeichnen
        day_of_week = tm.tm_wday;
        // Sonntag...
        if (day_of_week == 0) {
            day_of_week = 7;
        }
        Serial.println(day_of_week);
        rotation = gfx->getRotation();
        if ((day_of_week != old_day_of_week) || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_WEEK, center - OFFSET_RI_DAY_OF_WEEK, 0.0, 360.0, COLOR_CLEAR);
                gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_WEEK, center - OFFSET_RI_DAY_OF_WEEK, 0.0 + ORIENTATION_DEG, (360.0 * day_of_week)/7 + ORIENTATION_DEG, COLOR_DAY_OF_WEEK);
                draw_round_clock_mark(center - OFFSET_RO_DAY_OF_WEEK, center - OFFSET_RI_DAY_OF_WEEK, 7, 8);
                xSemaphoreGive(mutex_display);
            }
            old_day_of_week = day_of_week;
            oldRotation = rotation;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
void task_display_day_of_month_circle(void *parameter)
{
    time_t now;
    tm tm;
    int old_day_of_month = -1;
    int day_of_month;
    int max_day_of_month;
    int oldRotation = -1;
    int rotation; 
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);    
        // Tageskreis (Datum) zeichnen
        day_of_month = tm.tm_mday;
        max_day_of_month = days_in_month(tm.tm_year, tm.tm_mon);
        rotation = gfx->getRotation();
        if ((day_of_month != old_day_of_month) || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_MONTH, center - OFFSET_RI_DAY_OF_MONTH, 0.0, 360.0, COLOR_CLEAR);
                gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_MONTH, center - OFFSET_RI_DAY_OF_MONTH, 0.0 + ORIENTATION_DEG, (360.0 * day_of_month)/max_day_of_month + ORIENTATION_DEG, COLOR_DAY_OF_MONTH);
                draw_round_clock_mark(center - OFFSET_RO_DAY_OF_MONTH, center - OFFSET_RI_DAY_OF_MONTH, max_day_of_month, 5);
                xSemaphoreGive(mutex_display);
            }
            old_day_of_month = day_of_month;
            oldRotation = rotation;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
void task_display_month_circle(void *parameter)
{
    time_t now;
    tm tm;
    int month;
    int old_month = -1;
    int oldRotation = -1;
    int rotation;     
    // Task-Loop
    while(1) {
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);
        // Monatskeis zeichnen
        month = tm.tm_mon + 1;
        rotation = gfx->getRotation();         
        if ((month != old_month) || (oldRotation != rotation)) {
            if (xSemaphoreTake(mutex_display, 1000)) {
                gfx->fillArc(center, center, center - OFFSET_RO_MONTH, center - OFFSET_RI_MONTH, 0.0, 360.0, COLOR_CLEAR);
                gfx->fillArc(center, center, center - OFFSET_RO_MONTH, center - OFFSET_RI_MONTH, 0.0 + ORIENTATION_DEG, (360.0 * month)/12 + ORIENTATION_DEG, COLOR_MONTH);
                draw_round_clock_mark(center - OFFSET_RO_MONTH, center - OFFSET_RI_MONTH, 12, 5);
                xSemaphoreGive(mutex_display);
            }
            old_month = month;
            oldRotation = rotation;
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
void task_display_time_is_sync_circle(void *parameter)
{
    uint16_t color;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_last_sync = tv.tv_sec;    
    // Task-Loop
    while(1) {
        // Display-Ausrichtung ist egal, da vollständiger Kreis...
        if ((tv.tv_sec - time_last_sync) < 1920) {         // 1920s -> 32min
            color = COLOR_TIMESTATUS_GOOD;
        } else {
            color = COLOR_TIMESTATUS_BAD;
            time_is_sync = false;
        }
        if (xSemaphoreTake(mutex_display, 1000)) {
            gfx->fillArc(center, center, center - OFFSET_RO_TIMESTATUS, center - OFFSET_RI_TIMESTATUS, 0.0, 360.0, color);
            xSemaphoreGive(mutex_display);
        }
        // suspend task?
        if (cheat_mode == true) vTaskSuspend(NULL);
        vTaskDelay(1000 / portTICK_PERIOD_MS);    // jede Sekunde reicht
    }
}

// *********************************************************************
bool is_task_suspended(TaskHandle_t handle)
{
    return (eTaskGetState(handle) == eSuspended);
}

// *********************************************************************
void task_display_cheat_mode(void *parameter)
{
    time_t now;
    tm tm;
    char buf[20];
    // Task-Loop
    while(1) {
        // suspend task?
        if (cheat_mode == false) vTaskSuspend(NULL);
        // aktuelle Zeit ermitteln
        time(&now);
        localtime_r(&now, &tm);
        // nur dann etwas ausgeben, wenn die anderen _display_-Tasks suspended
        if (is_task_suspended(handle_task_display_second_circle) &&
            is_task_suspended(handle_task_display_minute_circle) &&
            is_task_suspended(handle_task_display_hour_circle) &&
            is_task_suspended(handle_task_display_sun_circle) &&
            is_task_suspended(handle_task_display_day_of_week_circle) &&
            is_task_suspended(handle_task_display_day_of_month_circle) &&
            is_task_suspended(handle_task_display_month_circle) &&
            is_task_suspended(handle_task_display_time_is_sync_circle)        
           ) { 
                if (xSemaphoreTake(mutex_display, 1000)) {
                    gfx->fillScreen(BACKGROUND);
                    gfx->setTextSize(2);
                    strftime(buf, sizeof(buf), "%A", &tm);
                    gfx->setCursor(40, 60);
                    gfx->println(buf);
                    strftime(buf, sizeof(buf), "%d.%m.%Y", &tm);
                    gfx->setCursor(40, 80);
                    gfx->println(buf);
                    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
                    gfx->setCursor(40, 100);
                    gfx->println(buf);
                    xSemaphoreGive(mutex_display);
                }     
        }   
        vTaskDelay(1000 / portTICK_PERIOD_MS);    // jede Sekunde reicht
    }
}

// *********************************************************************
void task_set_rotation(void *parameter)
{
    int rot;
    int old_rot = -1;
    bool old_cheat_mode = false;
    // Task-Loop 
    while(1) {
        rot = get_display_rotation();
        if (rot != old_rot) {
            // ...mutex?
            gfx->setRotation(rot); 
            old_rot = rot;
            // Cheat-Mode?
            if (rot == 1) {
                cheat_mode = true;
            } else {
                cheat_mode = false;
            }
            // hat sich cheat-mode geändert?
            if (old_cheat_mode != cheat_mode) {
                old_cheat_mode = cheat_mode;
                // Resumes entsprechender Tasks
                if (cheat_mode == true) {
                   // CHEAT-MODE
                   vTaskResume(handle_task_display_cheat_mode); 
                } else {
                    // CIRCLECLOCK
                    // Display löschen
                    if (xSemaphoreTake(mutex_display, 1000)) {
                        gfx->fillScreen(BACKGROUND);
                        xSemaphoreGive(mutex_display);
                    }               
                    vTaskResume(handle_task_display_second_circle);
                    vTaskResume(handle_task_display_minute_circle);
                    vTaskResume(handle_task_display_hour_circle);
                    vTaskResume(handle_task_display_sun_circle);
                    vTaskResume(handle_task_display_day_of_week_circle);
                    vTaskResume(handle_task_display_day_of_month_circle);
                    vTaskResume(handle_task_display_month_circle);
                    vTaskResume(handle_task_display_time_is_sync_circle);
                }
            }
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

// *********************************************************************
int get_display_rotation()
{
    QMI8658_Data s;
    static int rot = 0;
    // Sensordaten auslesen
    if (imu.readSensorData(s)) {
        // Richtung bestimmen
        if        (s.accelY < -800) {
            rot = 0;
        } else if (s.accelX > 800) {
            rot = 1;        
        } else if (s.accelY > 800) {
            rot = 2;        
        } else if (s.accelX < -800) {
            rot = 3;        
        }         
    }
    // Rückgabewert ist momentane Ausrichtung
    return rot;
}

// ********************************************************************************
void setup(void)
{
    
    Serial.begin(115200);
    
    Serial.println("**************");
    Serial.println("Setup beginnt!");
    
    // Initialisierung QMI8658
    Serial.println("QMI8658 initialisieren..."); 
    if (!imu.begin(6, 7)) {
        Serial.println("Fehler bei Initialisierung QMI8658...");
        delay(5000);
        ESP.restart();
    }
    Serial.println("QMI8658 erfolgreich initialisert!"); 
    Serial.println("Konfiguration QMI8658...");
    imu.setAccelRange(QMI8658_ACCEL_RANGE_8G);  // Set accelerometer range (±8g)
    imu.setAccelODR(QMI8658_ACCEL_ODR_1000HZ);  // Set accelerometer output data rate (1000Hz)
    imu.setAccelUnit_mg(true);                  // Use mg (like your screen: ACC_X = -965.82)
    Serial.println("QMI8658 (ACCEL) einschalten..."); 
    imu.enableSensors(QMI8658_ENABLE_ACCEL); 
    delay(200);   

    // Initialisierung Display
    gfx->begin();
    gfx->fillScreen(BACKGROUND);
    // ...tft-backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    // ...init LCD constant
    w = gfx->width();
    h = gfx->height();
    if (w < h)
        center = w / 2;
    else
        center = h / 2;
    // Display-Ausgaben nach Physik ausrichten
    gfx->setRotation(get_display_rotation());
    
    // Initialisierung Boot-Button plus Start entspr. Task (damit ab jetzt darauf reagiert werden kann)
    pinMode(BOOT_BTN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOOT_BTN), boot_btn_isr, CHANGE);
    xTaskCreatePinnedToCore(task_boot_button, "task_boot_button", 4096, NULL, 1, NULL, 1);
    
    // WiFi via WiFiManager
    display_setup_message("Initialisierung...");
    WiFi.mode(WIFI_STA);
    
    bool res;
    display_setup_message("Wifi-Setup via Handy etc.!");
    display_setup_message("-> WLAN : circleclock");
    display_setup_message("-> AP-IP: 192.168.4.1");
    res = wm.autoConnect("circleclock");           // anonymous ap
    if(!res) {
        Serial.println("Fehler WLAN :-(");
        display_setup_message("Fehler WLAN :-(");
        delay(3000);
        gfx->fillScreen(BACKGROUND);
        ESP.restart();
    } else {
        // ...hier sind wir mit dem WLAN verbunden
        Serial.println("Verbunden :-)");
        display_setup_message("Verbunden :-)");
    }

    // NTP-Client initialisieren, Zeitzone, etc.
    // ...lokale Zeitzone setzen
    setenv("TZ", MY_TZ, 1);
    tzset();
    // ...SNTP konfigurieren
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // ...mehrere NTP-Server (als Fallback)
    sntp_setservername(0, "ptbtime1.ptb.de");
    sntp_setservername(1, "ptbtime2.ptb.de");
    sntp_setservername(2, "ptbtime3.ptb.de");
    sntp_setservername(3, "de.pool.ntp.org");
    // ...NTP-Sync-Intervall
    sntp_set_sync_interval(30 * 60 * 1000);             // 30min
    // ...Smooth Sync dauerhaft aktivieren
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    // ... ein Callback registrieren (--> erstmals aktuelle Zeit von NTP-Server)
    sntp_set_time_sync_notification_cb(time_sync_cb);
    // ...SNTP starten
    sntp_init(); 

    // auf Zeitsynchronisation warten
    Serial.println("Warte auf NTP-Sync.");
    display_setup_message("Warte auf NTP-Sync.");
    while (time_is_sync == false) {
       delay(10);
    }
    
    // Display löschen
    gfx->fillScreen(BACKGROUND);
    
    // FreeRTOS Mutexe...
    mutex_display = xSemaphoreCreateMutex();
    if (mutex_display == NULL) {
        Serial.println("Problem bei Erzeugung mutex_display!");
        while (1);
    }
    
    // FreeRTOS-Tasks initialisieren/starten (Heap-Größen könnten noch optimiert werden...)
    xTaskCreatePinnedToCore(task_set_rotation, "task_set_rotation", 10000, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(task_display_second_circle, "task_display_second_circle", 10000, NULL, 1, &handle_task_display_second_circle, 1);
    xTaskCreatePinnedToCore(task_display_minute_circle, "task_display_minute_circle", 10000, NULL, 1, &handle_task_display_minute_circle, 1);
    xTaskCreatePinnedToCore(task_display_hour_circle, "task_display_hour_circle", 10000, NULL, 1, &handle_task_display_hour_circle, 1);
    xTaskCreatePinnedToCore(task_display_sun_circle, "task_display_sun_circle", 10000, NULL, 1, &handle_task_display_sun_circle, 1);
    xTaskCreatePinnedToCore(task_display_day_of_week_circle, "task_display_day_of_week_circle", 10000, NULL, 1, &handle_task_display_day_of_week_circle, 1);
    xTaskCreatePinnedToCore(task_display_day_of_month_circle, "task_display_day_of_month_circle", 10000, NULL, 1, &handle_task_display_day_of_month_circle, 1);
    xTaskCreatePinnedToCore(task_display_month_circle, "task_display_month_circle", 10000, NULL, 1, &handle_task_display_month_circle, 1);
    xTaskCreatePinnedToCore(task_display_time_is_sync_circle, "task_display_time_is_sync_circle", 10000, NULL, 1, &handle_task_display_time_is_sync_circle, 1);
    xTaskCreatePinnedToCore(task_display_cheat_mode, "task_display_cheat_mode", 10000, NULL, 1, &handle_task_display_cheat_mode, 1);
    // ...und letztere gleich wieder auf Suspend setzen; 
    // ...das nächste Mal werden wir mal ein wenig mit Gruppen-Events (FreeRTOS) 
    // experimentieren ;-) 
    vTaskSuspend(handle_task_display_cheat_mode);

    Serial.println("Setup ist durch!");

}

// ********************************************************************************
// ********************************************************************************
// ********************************************************************************
void loop()
{
    // wir arbeiten mit FreeRTOS-Task...
}
