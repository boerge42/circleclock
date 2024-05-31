/* ****************************************************************************************
 * 
 *   circle clock
 * ================
 * Uwe Berger; 2024
 * 
 * 
 * Hardware:
 * ---------
 *   --> https://www.waveshare.com/esp32-s3-lcd-1.28.htm
 *   --> https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28
 *
 *  
 * Program Sequence:
 * -----------------
 * * initialization hardware
 * * connection/registration wlan
 * * synchronization time via ntp
 * * shutdown of wlan hardware
 * * cyclic measurement of battery voltage
 * * cyclic activation wlan
 *   * time synchronization
 *   * write collected voltage measurements into an influxDB
 *   * shutdown of wlan hardware
 * 
 * 
 * Clock (circle from outside to inside):
 * --------------------------------------
 * * second
 * * minute
 * * hour (24 hour division)
 * * sunrise/sunset
 * * day of the week
 * * day of the month
 * * month of the year
 * * batterylevel
 * * time is sync with a ntp-server
 * 
 * 
 * ToDo:
 * -----
 * * make influxDB stuff switchable
 * 
 * 
 * =========
 * Have fun! 
 * 
 * ****************************************************************************************
*/

#include <WiFi.h>
#include <ezTime.h>
#include <Arduino_GFX_Library.h>
#include <InfluxDbClient.h>
#include "SensorQMI8658.hpp"
#include "astro.h"

// display pins on esp
#define TFT_CS 9
#define TFT_DC 8
#define TFT_RST 12
#define TFT_SCK 10
#define TFT_MOSI 11
#define TFT_MISO -1  // no data coming back
#define TFT_BL 40

#if defined(DISPLAY_DEV_KIT)
Arduino_GFX *gfx = create_default_Arduino_GFX();
#else /* !defined(DISPLAY_DEV_KIT) */

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GC9A01 *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

#endif /* !defined(DISPLAY_DEV_KIT) */

// QMI8658
#define IMU_SDA  6
#define IMU_SCL  7
#define IMU_IRQ  -1

SensorQMI8658 qmi;

// circles...
// ...colors
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
// ...radius
#define OFFSET_RO_SECOND        0
#define OFFSET_RI_SECOND        4
#define OFFSET_RO_MINUTE        5
#define OFFSET_RI_MINUTE        24
#define OFFSET_RO_HOUR          24
#define OFFSET_RI_HOUR          43
#define OFFSET_RO_SUN           44
#define OFFSET_RI_SUN           48
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

// pi
#define PI  3.1415926536

// WiFi
#define WIFI_SSID     "UWE_HOME1"
#define WIFI_PASSWORD "u1504u1504"

// InfluxDB
#define INFLUX_URL    "http://nanotuxedo:8086/"
#define INFLUX_DB     "circleclock"
// ...timestamp for influxdb
// Set timezone string according to https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
// Examples:
//  Pacific Time:   "PST8PDT"
//  Eastern:        "EST5EDT"
//  Japanesse:      "JST-9"
//  Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
// NTP servers the for time synchronization.
// For the fastest time sync find NTP servers in your area: https://www.pool.ntp.org/zone/
//     server 0.de.pool.ntp.org
//	   server 1.de.pool.ntp.org
//	   server 2.de.pool.ntp.org
//	   server 3.de.pool.ntp.org
//~ #define NTP_SERVER1  "pool.ntp.org"
//~ #define NTP_SERVER2  "time.nis.gov"
#define NTP_SERVER1  "0.de.pool.ntp.org"
#define NTP_SERVER2  "1.de.pool.ntp.org"


// ezTime & time zone
#define MY_TIME_ZONE "Europe/Berlin"
Timezone myTZ;
char my_time_zone[50] = MY_TIME_ZONE;

// miscellaneous
static int16_t w, h, center;

// task cycles
#define TASK_DISPLAY_BATTERY_LEVEL  60000       // in ms --> 60s
#define TASK_INFLUX_INSERT          10000       // in ms --> 10s
#define TASK_CONNECTION_WIFI        500         // in ms --> 500ms
#define TASK_IMU_READ		        250         // in ms --> 250ms
#define TFT_BACKLIGHT_OFF_INTERVAL  60000       // in ms --> 60s

// InfluxDB
InfluxDBClient client(INFLUX_URL, INFLUX_DB);
Point sensorStatus("battery");

// parameter influx-batch
#define WRITE_PRECISION             WritePrecision::S
#define MAX_BATCH_SIZE              70                    // 6 Werte in der Minute; alle 10min flush (plus etwas Puffer...)
#define WRITE_BUFFER_SIZE           (MAX_BATCH_SIZE * 2)
#define INFLUX_FLUSH_INTERVAL       660                   // in Sekunden -> 10min --> sollte groesser als WIFI_SYNC_INTERVAL sein

#define WIFI_SYNC_INTERVAL          600000                // in ms --> 10min
#define MAX_CONNECTION_ATTEMPTS     50                    // max. 50 Versuche (WiFi-Connect)

long last_wifi_connection = 0;
uint16_t connection_attempts = 0;

uint8_t last_wifi_channel;
uint8_t last_wifi_ap_mac[6];

long last_tft_backlight_on;

// **********************************************************************************************
void connectionWifi()
{
    static long old_millis = 0;
    if ((connection_attempts > 0) and (old_millis + TASK_CONNECTION_WIFI < millis())) {
        old_millis = millis();
        Serial.println("==> connectWifi()");
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("...not connected");
            // nach 5 fehlgeschlagenen Versuchen beim naechten Mal nur mit SSID/PWD versuchen
            if (MAX_CONNECTION_ATTEMPTS - connection_attempts == 5) {
                WiFi.disconnect(true);
                WiFi.disconnect(false);
                WiFi.mode(WIFI_STA);
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
                Serial.println("...forget last ap!");
            }
            connection_attempts--;
        } else {
            Serial.println("...connected");
            Serial.println(connection_attempts);
            connection_attempts = 0;                        // wifi-connect :-)
            timeSync(TZ_INFO, NTP_SERVER1, NTP_SERVER2);    // influx timesync
            client.flushBuffer();                           // influx write batch to db 
            events();										// sync. ezTime
            last_wifi_connection = millis();
            // aktuellen AP merken
            last_wifi_channel = WiFi.channel();
            memcpy(last_wifi_ap_mac, WiFi.BSSID(), 6);
            disableWiFi();
        }
    }
}

// **********************************************************************************************
void disableWiFi()
{
    Serial.println("==> disableWiFi()");
    WiFi.disconnect(true);                                  // Disconnect from the network
    WiFi.mode(WIFI_OFF);                                    // Switch WiFi off
    setCpuFrequencyMhz(40);
}

// **********************************************************************************************
void enableWiFi()
{
    if (last_wifi_connection + WIFI_SYNC_INTERVAL < millis()) {
        last_wifi_connection = millis();
        Serial.println("==> enableWiFi()");
        setCpuFrequencyMhz(240); 
        WiFi.disconnect(false);                                                           // Reconnect the network
        WiFi.mode(WIFI_STA);                                                              // Switch WiFi on
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, last_wifi_channel, last_wifi_ap_mac, true);  // erstmal mit bekannten AP
        connection_attempts = MAX_CONNECTION_ATTEMPTS;
    }
}

// **********************************************************************************************
int get_moving_avg(int newValue) 
{
	const int NUM_VALUES = 20;
	static int values[NUM_VALUES] = {0};
	static int currentIndex = 0;
	static int count = 0;
	static long sum = 0;
	sum -= values[currentIndex];
	sum += newValue;
	values[currentIndex] = newValue;
	currentIndex = (currentIndex + 1) % NUM_VALUES;
	if (count < NUM_VALUES) {
		count++;
	  }
	return sum / count;
}

// **********************************************************************************************
void influx_insert()
{
    static long old_millis = 0;
    if (old_millis + TASK_INFLUX_INSERT < millis()) {
        old_millis = millis();
        sensorStatus.clearFields();
        sensorStatus.setTime(time(nullptr));
        sensorStatus.addField("analogRead", get_moving_avg(analogRead(1)));
        Serial.print("==> influx_insert: ");
        Serial.println(client.pointToLineProtocol(sensorStatus));
        client.writePoint(sensorStatus);
    }
}

// **********************************************************************************************
void task_imu_read()
{
    static long old_millis = 0;
    static IMUdata old_gyr;
	IMUdata gyr;
    if (old_millis + TASK_IMU_READ < millis()) {
        old_millis = millis();
		if (qmi.getDataReady()) {
			if (qmi.getGyroscope(gyr.x, gyr.y, gyr.z)) {
				if ((abs(old_gyr.x - gyr.x) > 5)||(abs(old_gyr.y - gyr.y) > 5)||(abs(old_gyr.z - gyr.z) > 5)) {				// verbesserungswuerdig?
					last_tft_backlight_on = millis();
					old_gyr.x = gyr.x;
					old_gyr.y = gyr.y;
					old_gyr.z = gyr.z;
					Serial.println("task_backlight_on()");
					digitalWrite(TFT_BL, HIGH);
				}
			}
		}
    }
}

// **********************************************************************************************
void task_tft_backlight_off()
{
    if (last_tft_backlight_on + TFT_BACKLIGHT_OFF_INTERVAL < millis()) {
        last_tft_backlight_on = millis();
		Serial.println("task_backlight_off()");
        digitalWrite(TFT_BL, LOW);
	}
}

// **************************************************************
int get_battery_percent(void)
{
    int raw_voltage = get_moving_avg(analogRead(1));
    int percent;
    char buf[6];
    if (raw_voltage > 1620) {
		percent = 100;
	} else {
		percent = map(raw_voltage, 1620, 1200, 100, 0);
	}
	return percent;
}

// **************************************************************
void display_setup_message(String msg)
{
	static int y = 50;
	const int x = 40;
	const int dy = 15;
    gfx->setRotation(3);
    gfx->setTextSize(1);
    gfx->setCursor(x, y);
    gfx->println(msg);
    y = y + dy;		
    gfx->setRotation(2);
}

// ********************************************************************************
void draw_round_clock_mark(int16_t inner, int16_t outer, int16_t count, int16_t mark)
{
    float angle = 2 * PI / count;
    int i;
    uint16_t color;
    for (i = 0; i < count; i++) {
        if (i%mark != 0) {
            color = COLOR_MARK;
        } else {
            color = COLOR_SUBMARK;
        }
        gfx->drawLine(cos(i*angle)*inner+center, sin(i*angle)*inner+center, cos(i*angle)*outer+center, sin(i*angle)*outer+center, color);
    }
    gfx->drawCircle(center, center, inner, COLOR_MARK);
    gfx->drawCircle(center, center, outer, COLOR_MARK);
}

// ********************************************************************************
void task_display_second_circle()
{
    int currentSecond = myTZ.second();
    int currentMs = myTZ.ms();
	static float oldSecond_deg = 0;
    // circle second
    float currentSecond_deg = 360.0/60*currentSecond + 360.0/60*currentMs/1000;
    if ((currentSecond_deg < oldSecond_deg) && (currentSecond == 0)) {  // --> da scheint es ein Problem in ezTime zu geben!!! (Sekunde schaltet erst nach Millisekunde...)
        gfx->fillArc(center, center, center - OFFSET_RO_SECOND, center - OFFSET_RI_SECOND, 0.0, 360.0, COLOR_CLEAR);
        //~ draw_round_clock_mark(center - OFFSET_RO_SECOND, center - OFFSET_RI_SECOND, 60, 5);
        oldSecond_deg = 0.0;
    } else {
        if ((currentSecond_deg-oldSecond_deg) > 0.5) {
            gfx->fillArc(center, center, center - OFFSET_RO_SECOND, center - OFFSET_RI_SECOND, oldSecond_deg, currentSecond_deg, COLOR_SECOND);
            //~ draw_round_clock_mark(center - OFFSET_RO_SECOND, center - OFFSET_RI_SECOND, 60, 5);
            oldSecond_deg = currentSecond_deg;
        }
    }
}

// ********************************************************************************
void task_display_minute_circle()
{
    int currentMinute = myTZ.minute();
    int currentSecond = myTZ.second();
	static float oldMinute_deg = 0;
    // circle minute
    float currentMinute_deg = 360.0/60*currentMinute + 360.0/60*currentSecond/60;
    if (currentMinute_deg < oldMinute_deg) {
        gfx->fillArc(center, center, center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, 0.0, 360.0, COLOR_CLEAR);
        draw_round_clock_mark(center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, 60, 5);
        oldMinute_deg = 0.0;
    } else {
        if ((currentMinute_deg > oldMinute_deg)  && ((currentMinute_deg-oldMinute_deg) > 1.0)) {
            gfx->fillArc(center, center, center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, oldMinute_deg, currentMinute_deg, COLOR_MINUTE);
            draw_round_clock_mark(center - OFFSET_RO_MINUTE, center - OFFSET_RI_MINUTE, 60, 5);
            oldMinute_deg = currentMinute_deg;
        }
    }
}

// ********************************************************************************
void task_display_hour_circle()
{
    int currentHour = myTZ.hour();
    int currentMinute = myTZ.minute();
    static float oldHour_deg = 0;
    // circle hour
    float currentHour_deg = 360.0/24*currentHour + 360.0/24*currentMinute/60;
    if (currentHour_deg < oldHour_deg) {
        gfx->fillArc(center, center, center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, 0.0, 360.0, COLOR_CLEAR);
        draw_round_clock_mark(center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, 24, 3);
        oldHour_deg = 0.0;
    } else {
        if ((currentHour_deg > oldHour_deg) && ((currentHour_deg-oldHour_deg) > 1.0)) {
            gfx->fillArc(center, center, center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, oldHour_deg, currentHour_deg, COLOR_HOUR);
            draw_round_clock_mark(center - OFFSET_RO_HOUR, center - OFFSET_RI_HOUR, 24, 3);
            oldHour_deg = currentHour_deg;
        }
    }
}

// ********************************************************************************
void task_display_sun_circle()
{
    uint16_t currentDayOfYear = myTZ.dayOfYear() + 1;
    boolean currentIsDST = myTZ.isDST();
	static uint16_t oldDayOfYear = 0;
    // sun (& date)
    if (currentDayOfYear != oldDayOfYear) {
        // sun
        gfx->fillArc(center, center, center - OFFSET_RO_SUN, center - OFFSET_RI_SUN, 0.0, 360.0, COLOR_CLEAR);
        float currentSunrise = sunrise(currentDayOfYear, currentIsDST);
        int sunriseHour = int(currentSunrise);
        int sunriseMinute = round((currentSunrise - sunriseHour) * 60.0);
        float sunrise_deg = 360.0/24*sunriseHour + 360.0/24*sunriseMinute/60;
        float currentSunset = sunset(currentDayOfYear, currentIsDST);
        int sunsetHour = int(currentSunset);
        int sunsetMinute = round((currentSunset - sunsetHour) * 60.0);   
        float sunset_deg = 360.0/24*sunsetHour + 360.0/24*sunsetMinute/60;
        gfx->fillArc(center, center, center - OFFSET_RO_SUN, center - OFFSET_RI_SUN, sunrise_deg, sunset_deg, COLOR_SUN);
        oldDayOfYear = currentDayOfYear;
    }
}

// ********************************************************************************
void task_display_day_of_week_circle()
{
    static int old_day_of_week = 0;
    int day_of_week = myTZ.dateTime("N").toInt();
    if (day_of_week != old_day_of_week) {
        old_day_of_week = day_of_week;
        gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_WEEK, center - OFFSET_RI_DAY_OF_WEEK, 0.0, 360.0, COLOR_CLEAR);
        gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_WEEK, center - OFFSET_RI_DAY_OF_WEEK, 0.0, (360.0 * day_of_week)/7, COLOR_DAY_OF_WEEK);
        draw_round_clock_mark(center - OFFSET_RO_DAY_OF_WEEK, center - OFFSET_RI_DAY_OF_WEEK, 7, 8);
    }
}

// ********************************************************************************
void task_display_day_of_month_circle()
{
    static int old_day_of_month = 0;
    int day_of_month = myTZ.dateTime("d").toInt();
    int max_day_of_month = myTZ.dateTime("t").toInt();
    if (day_of_month != old_day_of_month) {
        old_day_of_month = day_of_month;
        gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_MONTH, center - OFFSET_RI_DAY_OF_MONTH, 0.0, 360.0, COLOR_CLEAR);
        gfx->fillArc(center, center, center - OFFSET_RO_DAY_OF_MONTH, center - OFFSET_RI_DAY_OF_MONTH, 0.0, (360.0 * day_of_month)/max_day_of_month, COLOR_DAY_OF_MONTH);
        draw_round_clock_mark(center - OFFSET_RO_DAY_OF_MONTH, center - OFFSET_RI_DAY_OF_MONTH, max_day_of_month, 5);
    }
}

// ********************************************************************************
void task_display_month_circle()
{
    static int old_month = 0;
    int month = myTZ.dateTime("m").toInt();
    if (month != old_month) {
        old_month = month;
        gfx->fillArc(center, center, center - OFFSET_RO_MONTH, center - OFFSET_RI_MONTH, 0.0, 360.0, COLOR_CLEAR);
        gfx->fillArc(center, center, center - OFFSET_RO_MONTH, center - OFFSET_RI_MONTH, 0.0, (360.0 * month)/12, COLOR_MONTH);
        draw_round_clock_mark(center - OFFSET_RO_MONTH, center - OFFSET_RI_MONTH, 12, 5);
    }
}

// ********************************************************************************
void task_display_battery_circle()
{
    int battery_level = get_battery_percent();
    static int old_battery_level = 0;
    static long old_millis = 0;
    uint16_t color;
    if ((old_millis + TASK_DISPLAY_BATTERY_LEVEL < millis()) || (old_millis == 0)) {
        old_millis = millis();
        if (battery_level != old_battery_level) {
            old_battery_level = battery_level;
            if (battery_level > 25) {
                color = COLOR_BATTERY_GOOD; 
            } else { 
                color = COLOR_BATTERY_BAD;
            }
            gfx->fillArc(center, center, center - OFFSET_RO_BATTERY, center - OFFSET_RI_BATTERY, 0.0, 360.0, COLOR_CLEAR);
            gfx->fillArc(center, center, center - OFFSET_RO_BATTERY, center - OFFSET_RI_BATTERY, 0.0, (360.0 * battery_level)/100, color);
        }
    }
}

// ********************************************************************************
void task_display_timestatus_circle()
{
    static int old_timestatus = -42;
    int timestatus = timeStatus();
    uint16_t color;
    if (timestatus != old_timestatus) {
        old_timestatus = timestatus;
        if (timestatus == timeSet) {
            color = COLOR_TIMESTATUS_GOOD;
        } else {
            color = COLOR_TIMESTATUS_BAD;
        }
        gfx->fillArc(center, center, center - OFFSET_RO_TIMESTATUS, center - OFFSET_RI_TIMESTATUS, 0.0, 360.0, color);
    }
}

// ********************************************************************************
void setup(void)
{
    
    // init serial
    Serial.begin(115200);
    Serial.println();

    // init display
    gfx->begin();
    gfx->setRotation(2);
    gfx->fillScreen(BACKGROUND);
	// ...tft-backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    last_tft_backlight_on = millis();
    // ...init LCD constant
    w = gfx->width();
    h = gfx->height();
    if (w < h)
        center = w / 2;
    else
        center = h / 2;

    display_setup_message("Setup...");
  
    // connect to Wi-Fi
    display_setup_message(" --> connect Wifi...");
    Serial.println("WiFi:");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("");
    // aktuellen AP merken
    last_wifi_channel = WiFi.channel();
    memcpy(last_wifi_ap_mac, WiFi.BSSID(), 6);
    
    // synchronize date/time 
    display_setup_message(" --> sync. ezTime...");
    waitForSync();
    Serial.println("UTC: " + UTC.dateTime());
    myTZ.setLocation(my_time_zone);
    Serial.println("MyTZ time: " + myTZ.dateTime("d-M-y H:i:s"));        
    
    // Accurate time is necessary for certificate validation and writing in batches
    // Syncing progress and the time will be printed to Serial.
    display_setup_message(" --> sync. influxDB time...");
    timeSync(TZ_INFO, NTP_SERVER1, NTP_SERVER2);

	// QMI8658
    display_setup_message(" --> init QMI8658...");
	qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IMU_SDA, IMU_SCL);
    //qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz, SensorQMI8658::LPF_MODE_0, true);
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_64DPS, SensorQMI8658::GYR_ODR_896_8Hz, SensorQMI8658::LPF_MODE_3, true);
    // In 6DOF mode (accelerometer and gyroscope are both enabled),
    // the output data rate is derived from the nature frequency of gyroscope
    qmi.enableGyroscope();
    //qmi.enableAccelerometer();
        
    // influxDB
    // Enable messages batching and retry buffer
    client.setWriteOptions(WriteOptions().writePrecision(WRITE_PRECISION).batchSize(MAX_BATCH_SIZE).bufferSize(WRITE_BUFFER_SIZE).flushInterval(INFLUX_FLUSH_INTERVAL));
    
    // switch off wifi
    disableWiFi();
    last_wifi_connection = millis();
    connection_attempts = 0;

    display_setup_message("Ready!");
	delay(1000);
	
	// clear display
    gfx->fillCircle(center, center, center, COLOR_CLEAR);

}

// ********************************************************************************
// ********************************************************************************
// ********************************************************************************
void loop()
{
    // a few tasks...
    // ...display
    task_display_second_circle();
    task_display_minute_circle();
    task_display_hour_circle();
    task_display_sun_circle();
    task_display_day_of_week_circle();
    task_display_day_of_month_circle();
    task_display_month_circle();
    task_display_battery_circle();
    task_display_timestatus_circle();
    // ...other
    task_imu_read();
    task_tft_backlight_off();
    influx_insert();
    enableWiFi();
    connectionWifi();
    // a little breather ;-)
    delay(50);
}
