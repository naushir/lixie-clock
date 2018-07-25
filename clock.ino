#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Timezone.h>
#include <EEPROM.h>
#include "Lixie.h"

#define DATA_PIN    5
#define NUM_LIXIES  6
static Lixie lix(DATA_PIN, NUM_LIXIES);

// United Kingdom (London, Belfast)
static TimeChangeRule BST = {"BST", Last, Sun, Mar, 1, 60};        // British Summer Time
static TimeChangeRule GMT = {"GMT", Last, Sun, Oct, 2, 0};         // Standard Time
static Timezone UK(BST, GMT);
// UTC
static TimeChangeRule utcRule = {"UTC", Last, Sun, Mar, 1, 0};     // UTC
static Timezone UTC(utcRule);
// India Standard Time
TimeChangeRule ind = {"IND", Last, Sun, Mar, 1, 330};
Timezone IND(ind);
// Australia Eastern Time Zone (Sydney, Melbourne)
TimeChangeRule aEDT = {"AEDT", First, Sun, Oct, 2, 660};    // UTC + 11 hours
TimeChangeRule aEST = {"AEST", First, Sun, Apr, 3, 600};    // UTC + 10 hours
Timezone AUSET(aEDT, aEST);


static char ssid[32] = "";
static char pass[32] = "";
static unsigned int localPort = 2390;

static IPAddress timeServerIP;
//const char* ntpServerName = "us.pool.ntp.org";
static const char* ntpServerName = "europe.pool.ntp.org";
static const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
static byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
static WiFiUDP udp;  // A UDP instance to let us send and receive packets over UDP

static time_t uptime = 0;
static time_t lastDate = 0;
static time_t lastDisplay = 0;
static time_t lastSync = 0;
static time_t lastStatus = 0;

// Maximum allowable minutes without syncing in secs.
static const time_t maxSyncTimeout = 360*SECS_PER_MIN;
// Hour/minuite to switch night mode on (24hr, must be greather than 12)
#define NIGHT_HOUR      23
#define NIGHT_MIN       00
// Hour/minuite to switch day mode on (must be less than 12)
#define DAY_HOUR         6
#define DAY_MIN         00
// Freqency of date view (min)
#define DATE_VIEW        4
// Duration of date view (secs)
#define DATE_DURATION   15
// Period to write out the clock status (secs).
#define STATUS_TIME     30

static bool outOfSync = false;
static bool nightMode = false;
static bool WifiDisconnected = false;
static bool dateMode = false;

void setup()
{
    Serial.begin(115200);
    Serial.println();

    lix.begin();
    // 5V/3A allowed.
    lix.max_power(5, 3000);
    lix.nixie_mode(true, true);

    loadCredentials();
    connectToWifi();
    setupOTA();

    // Get initial time, and setup sync updates.
    // Start with a refresh period of 1s until we succeed.
    setSyncInterval(1);
    setSyncProvider(getNtpTime);
    while (timeStatus() != timeSet)
        yield();

    // Resync period is now longer.
    setSyncInterval(27*SECS_PER_MIN);
    uptime = lastDate = now();
}

void loop()
{
    ArduinoOTA.handle();

    time_t t = now();
    displayClock(t);
    displayDate(t);
    writeStatus(t);
    checkTimeSync(t);
    checkDayNight(t);
    checkWifi();
}

static void checkTimeSync(time_t t)
{
    if (!outOfSync && ((t - lastSync) >= maxSyncTimeout))
    {
        lix.nixie_mode(false, false);
        lix.color(CRGB(255,0,0));
        outOfSync = true;
        Serial.println("Out of time sync!");
    }
    else if (outOfSync && ((t - lastSync) < maxSyncTimeout))
    {
        lix.nixie_mode(true, true);
        outOfSync = false;
        Serial.println("Time re-sync successful.");
    }
}

static void checkDayNight(time_t t)
{
    time_t local = UK.toLocal(t);
    if (!nightMode && (hour(local) >= NIGHT_HOUR && minute(local) >= NIGHT_MIN) 
        && hour(local) >= 12)
    {
        lix.brightness(64);
        nightMode = true;
        Serial.println("Night mode.");
    }
    else if (nightMode && (hour(local) >= DAY_HOUR && minute(local) >= DAY_MIN)
             && hour(local) < 12)
    {
        lix.brightness(255);
        nightMode = false;
        Serial.println("Day mode.");
    }
}

static void displayDate(time_t t)
{
    time_t local = UK.toLocal(lastDisplay);
    if (DATE_VIEW && !dateMode && ((t - lastDate) >= DATE_VIEW*SECS_PER_MIN)
        && (second(local) >= 10) && (second(local) <= 15))
    {
        lix.roll_out(100, 0);
        lix.nixie_mode(false);
        lix.color(CRGB(200,0,200));
        // Allow for any padding needed.
        uint32_t date = 1000000;
        date += day(t)*10000;
        date += month(t)*100;
        date += year(t) % 100;
        // Write to the display.
        //lix.roll_in(sum, 100);
        lix.waterfall(date, 400, 2500, 0);
        dateMode = true;
        lastDate = t;
    }
    else if (dateMode && ((t - lastDate) >= DATE_DURATION))
    {
        lix.roll_out(100, 1);
        lix.nixie_mode(true, true);
        dateMode = false;
    }
}

static void displayClock(time_t t)
{
    if (!dateMode && (t != lastDisplay))
    {
        lastDisplay = t;
        time_t local = UK.toLocal(lastDisplay);
        // Allow for any padding needed.
        uint32_t time = 1000000;
        // Put the hour on two digits,
        time += hourFormat12(local)*10000;
        // The minute on two more,
        time += minute(local)*100;
        // and the seconds on two more.
        time += second(local);
        // Write to the display.
        int fade_time = 400, roll_digits = 0;
        if ((second(local) % 10) == 0)
        {
            fade_time = 175;
            roll_digits = 4;
        }
        lix.write_crossfade(time, fade_time, roll_digits);
    }
    else
        yield();
}

static void checkWifi()
{
    if (!WifiDisconnected && (WiFi.isConnected() == false))
    {
        lix.nixie_mode(false, false);
        lix.color(CRGB(255,255,0));
        WifiDisconnected = true;
        WiFi.reconnect();
        Serial.println("WiFi disconnected.");
    }
    else if (WifiDisconnected && (WiFi.isConnected() == true))
    {
        lix.nixie_mode(true, true);
        WifiDisconnected = false;
        Serial.println("WiFi reconnected.");
    }
}

static void writeStatus(time_t t)
{
    // Write clock status to the serial port periodically.
    if (STATUS_TIME && ((t - lastStatus) >= STATUS_TIME))
    {
        time_t local = UK.toLocal(t);
        Serial.printf("[Debug] Local time: %02d:%02d:%02d - UTC epoch %lu, uptime: %lu\n", hour(local), minute(local), second(local), t, t-uptime);
        Serial.printf("[Debug] sync: %d, night: %d, WifiDisconnected: %d, date: %d\n",
                        outOfSync, nightMode, WifiDisconnected, dateMode);
        Serial.printf("[Debug] lastDate: %lu, lastDisplay: %lu, lastSync: %lu, lastStatus: %lu\n",
                        lastDate, lastDisplay, lastSync, lastStatus);
        lastStatus = t;
    }
}

static void loadCredentials(void)
{
    EEPROM.begin(512);
    EEPROM.get(0, ssid);
    EEPROM.get(sizeof(ssid), pass);
    EEPROM.end();
}

static void connectToWifi(void)
{
    // We start by connecting to a WiFi network
    Serial.print("Connecting to ");
    Serial.println(ssid);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    while (WiFi.status() != WL_CONNECTED)
        lix.sweep(CRGB(0,50,255), 5);

    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());

    WiFi.setAutoReconnect(true);

    Serial.println("Starting UDP");
    udp.begin(localPort);
    Serial.print("Local port: ");
    Serial.println(udp.localPort());
}

static void setupOTA(void)
{
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("OTA ready.");
}

static void rainbow(void)
{
    lix.nixie_mode(false);
    uint8_t hue = 0;
    for(int i = 50; i > 0; i--){
        lix.rainbow(hue,50);
        lix.write(random(1000000,1999999));
        hue+=20;
        delay(10);
    }
    lix.nixie_mode(true, true);
}

time_t getNtpTime(void)
{
    Serial.print("Getting NTP time.");
    //get a random server from the pool
    WiFi.hostByName(ntpServerName, timeServerIP); 
    Serial.print(ntpServerName);
    Serial.print(": ");
    Serial.println(timeServerIP);
    while (udp.parsePacket() > 0) ; // discard any previously received packets
    sendNTPpacket(timeServerIP); // send an NTP packet to a time server

    unsigned long t = millis();
    int size = 0;
    int counter = 20;
    do
    {
        rainbow();
        size = udp.parsePacket();
        counter--;
    } while (counter > 0 && size == 0);

    if (size)
    {
        Serial.print("packet received, length=");
        Serial.println(size);
        // We've received a packet, read the data from it
        udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
        //the timestamp starts at byte 40 of the received packet and is four bytes,
        // or two words, long. First, esxtract the two words:
        unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
        unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
        // combine the four bytes (two words) into a long integer
        // this is NTP time (seconds since Jan 1 1900):
        unsigned long secsSince1900 = highWord << 16 | lowWord;
        Serial.print("Seconds since Jan 1 1900 = " );
        Serial.println(secsSince1900);
        // now convert NTP time into everyday time:
        Serial.print("Unix time = ");
        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        const unsigned long seventyYears = 2208988800UL;
        // subtract seventy years:
        unsigned long epoch = secsSince1900 - seventyYears;
        // add any procssing delays (rounded up)
        epoch += (millis() - t + 500)/1000;
        // print Unix time:
        Serial.println(epoch);
        // Record last sync time.
        lastSync = epoch;
        return epoch;
    }
    else
    {
        Serial.println("NTP packet wait timeout.");
        return 0;
    }
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress& address)
{
    Serial.println("sending NTP packet...");
    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49;
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;

    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    udp.beginPacket(address, 123); //NTP requests are to port 123
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
}

void setButtonColor(int red, int green, int blue)
{
    red = 255 - red;
    green = 255 - green;
    blue = 255 - blue;
    analogWrite(RED_PIN, red);
    analogWrite(GREEN_PIN, green);
    analogWrite(BLUE_PIN, blue);  
}