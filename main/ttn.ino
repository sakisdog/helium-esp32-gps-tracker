/*

TTN module
Wrapper to use TTN with the LMIC library

Copyright (C) 2018 by Xose Pérez <xose dot perez at gmail dot com>

This code requires the MCCI LoRaWAN LMIC library
by IBM, Matthis Kooijman, Terry Moore, ChaeHee Won, Frank Rose
https://github.com/mcci-catena/arduino-lmic

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <hal/hal.h>
#include <SPI.h>
#include <vector>
#include <Preferences.h>
#include "configuration.h"
#include "credentials.h"

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

// LMIC GPIO configuration
const lmic_pinmap lmic_pins = {
    .nss = NSS_GPIO,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = RESET_GPIO,
    .dio = {DIO0_GPIO, DIO1_GPIO, DIO2_GPIO},
};


// Message counter, stored in RTC memory, survives deep sleep.
static RTC_DATA_ATTR uint32_t count = 0;

#ifdef USE_ABP
// These callbacks are only used in over-the-air activation, so they are
// left empty here (we cannot leave them out completely unless
// DISABLE_JOIN is set in config.h, otherwise the linker will complain).
void os_getArtEui (u1_t* buf) { }
void os_getDevEui (u1_t* buf) { }
void os_getDevKey (u1_t* buf) { }
#endif

#ifdef USE_OTAA
void os_getArtEui (u1_t* buf) { memcpy_P(buf, APPEUI, 8); }
void os_getDevEui (u1_t* buf) { memcpy(buf, DEVEUI, 8); }
void os_getDevKey (u1_t* buf) { memcpy_P(buf, APPKEY, 16); }
#endif

std::vector<void(*)(uint8_t message)> _lmic_callbacks;

// -----------------------------------------------------------------------------
// Private methods
// -----------------------------------------------------------------------------

void _ttn_callback(uint8_t message) {
    for (uint8_t i=0; i<_lmic_callbacks.size(); i++) {
        (_lmic_callbacks[i])(message);
    }
}

void forceTxSingleChannelDr() {
    // Disables all channels, except for the one defined by SINGLE_CHANNEL_GATEWAY
    // This only affects uplinks; for downlinks the default
    // channels or the configuration from the OTAA Join Accept are used.
    #ifdef SINGLE_CHANNEL_GATEWAY
    for(int i=0; i<9; i++) { // For EU; for US use i<71
        if(i != SINGLE_CHANNEL_GATEWAY) {
            LMIC_disableChannel(i);
        }
    }
    #endif

    // Set data rate (SF) and transmit power for uplink
    ttn_sf(LORAWAN_SF);
}


// DevEUI generator using devices's MAC address - from https://github.com/cyberman54/ESP32-Paxcounter/blob/master/src/lorawan.cpp
void gen_lora_deveui(uint8_t *pdeveui) {
    uint8_t *p = pdeveui, dmac[6];
    int i = 0;
    esp_efuse_mac_get_default(dmac);
    // deveui is LSB, we reverse it so TTN DEVEUI display
    // will remain the same as MAC address
    // MAC is 6 bytes, devEUI 8, set first 2 ones
    // with an arbitrary value
    *p++ = 0xFF;
    *p++ = 0xFE;
    // Then next 6 bytes are mac address reversed
    for (i = 0; i < 6; i++) {
        *p++ = dmac[5 - i];
    }
}


static void printHex2(unsigned v) {
    v &= 0xff;
    if (v < 16)
        Serial.print('0');
    Serial.print(v, HEX);
}

#ifdef USE_OTAA
// generate DevEUI from macaddr if needed
void initDevEUI() {
    bool needInit = true;
    for(int i = 0; i < sizeof(DEVEUI); i++)
        if(DEVEUI[i]) needInit = false;

    if(needInit)
        gen_lora_deveui(DEVEUI);

    Serial.print("\n\nDevEUI: ");
    for(int i = 0; i < sizeof(DEVEUI); i++) {
        if (i != 0)
                Serial.print("-");
        printHex2(DEVEUI[i]);
    }
    Serial.println();
}
#endif

// LMIC library will call this method when an event is fired
void onEvent(ev_t event) {
    switch(event) {
    case EV_JOINED: {
        #ifdef SINGLE_CHANNEL_GATEWAY
        forceTxSingleChannelDr();
        #endif

        // Disable link check validation (automatically enabled
        // during join, but because slow data rates change max TX
        // size, we don't use it in this example.
        if(!LORAWAN_ADR){
            LMIC_setLinkCheckMode(0); // Link check problematic if not using ADR. Must be set after join
        }

        Serial.println(F("EV_JOINED"));

        u4_t netid = 0;
        devaddr_t devaddr = 0;
        u1_t nwkKey[16];
        u1_t artKey[16];
        LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);
        Serial.print("netid: ");
        Serial.println(netid, DEC);
        Serial.print("devaddr: ");
        Serial.println(devaddr, HEX);
        Serial.print("AppSKey: ");
        for (size_t i=0; i<sizeof(artKey); ++i) {
            if (i != 0)
                Serial.print("-");
            printHex2(artKey[i]);
        }
        Serial.println("");
        Serial.print("NwkSKey: ");
        for (size_t i=0; i<sizeof(nwkKey); ++i) {
            if (i != 0)
                    Serial.print("-");
            printHex2(nwkKey[i]);
        }
        Serial.println();

        Preferences p;
        if(p.begin("lora", false)) {
            p.putUInt("netId", netid);
            p.putUInt("devAddr", devaddr);
            p.putBytes("nwkKey", nwkKey, sizeof(nwkKey));
            p.putBytes("artKey", artKey, sizeof(artKey));
            p.end();
        }
        // possibly not required, downlinks should work without this
        // LMIC_setPingable(1);
        break; }
    // Radio started transmitting
    case EV_TXSTART:
        Serial.println(F("* EVENT: RF Sending..."));
        break;
    // sending completed
    case EV_TXCOMPLETE:
        Serial.println(F("EV_TXCOMPLETE (inc. RX win. wait)"));
        if (LMIC.txrxFlags & TXRX_ACK) {
            Serial.println(F("*EVENT: Received ack"));
            _ttn_callback(EV_ACK);
        }
        Serial.print(F("LMIC datalen: "));
        Serial.println(LMIC.dataLen);
        if (LMIC.dataLen) {
            Serial.print(F("Data Received: "));
            Serial.write(LMIC.frame+LMIC.dataBeg, LMIC.dataLen);
            Serial.println();
            _ttn_callback(EV_RESPONSE);
        }
        break;
    // data received
    case EV_RXCOMPLETE:
        Serial.println(F("*EVENT: EV_RXCOMPLETE"));
        if (LMIC.dataLen) {
            Serial.print(F("Data Received: "));
            Serial.write(LMIC.frame+LMIC.dataBeg, LMIC.dataLen);
            Serial.println();
            _ttn_callback(EV_RESPONSE);
        }
        break;
    // beacon found by scanning
    // case EV_BEACON_FOUND:
    // // send empty frame up to notify server of ping mode and interval!
    //     LMIC_sendAlive();
    //     Serial.println("*EVENT: BEACON found - sendAlive");
    //     break;
    default:
        break;
    }

    // Send message callbacks
    _ttn_callback(event);
}

// -----------------------------------------------------------------------------
// Public methods
// -----------------------------------------------------------------------------

void ttn_register(void (*callback)(uint8_t message)) {
    _lmic_callbacks.push_back(callback);
}

size_t ttn_response_len() {
    return LMIC.dataLen;
}

void ttn_response(uint8_t * buffer, size_t len) {
    for (uint8_t i = 0; i < LMIC.dataLen; i++) {
        buffer[i] = LMIC.frame[LMIC.dataBeg + i];
    }
}

// If the value for LORA packet counts is unknown, restore from flash
static void initCount() {
  if(count == 0) {
    Preferences p;
    if(p.begin("lora", true)) {
        count = p.getUInt("count", 0);
        p.end();
    }
  }
}


bool ttn_setup() {
    initCount();

    #if defined(USE_OTAA)
        initDevEUI();
    #endif

    // SPI interface
    SPI.begin(SCK_GPIO, MISO_GPIO, MOSI_GPIO, NSS_GPIO);

    // LMIC init
    return ( 1 == os_init_ex( (const void *) &lmic_pins ) );
}

void ttn_join() {
    
    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

    #ifdef CLOCK_ERROR
    LMIC_setClockError(MAX_CLOCK_ERROR * CLOCK_ERROR / 100);
    #endif
        Serial.println("ttn_join is called here!");
        #if defined(CFG_eu868)

            // Set up the channels used by the Things Network, which corresponds
            // to the defaults of most gateways. Without this, only three base
            // channels from the LoRaWAN specification are used, which certainly
            // works, so it is good for debugging, but can overload those
            // frequencies, so be sure to configure the full frequency range of
            // your network here (unless your network autoconfigures them).
            // Setting up channels should happen after LMIC_setSession, as that
            // configures the minimal channel set.
            LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);      // g-band
            LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
            LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);      // g2-band
            Serial.println("EU868 frequency setup");
        #elif defined(CFG_us915)

            // NA-US channels 0-71 are configured automatically
            // but only one group of 8 should (a subband) should be active
            // TTN recommends the second sub band, 1 in a zero based count.
            // https://github.com/TheThingsNetwork/gateway-conf/blob/master/US-global_conf.json
            // in the US, with TTN, it saves join time if we start on subband 1
            // (channels 8-15). This will get overridden after the join by
            // parameters from the network. If working with other networks or in
            // other regions, this will need to be changed.
            LMIC_selectSubBand(1);
            Serial.println("US915 frequency setup");
        #elif defined(CFG_au915)

            // set sub band for AU915
            // https://github.com/TheThingsNetwork/gateway-conf/blob/master/AU-global_conf.json
            LMIC_selectSubBand(1);
            Serial.println("AU15 frequency setup");
        
        #endif

        // TTN defines an additional channel at 869.525Mhz using SF9 for class B
        // devices' ping slots. LMIC does not have an easy way to define set this
        // frequency and support for class B is spotty and untested, so this
        // frequency is not configured here.

        // Disable link check validation
        LMIC_setLinkCheckMode(0);

        #ifdef SINGLE_CHANNEL_GATEWAY
        forceTxSingleChannelDr();
        #else
        // Set default rate and transmit power for uplink (note: txpow seems to be ignored by the library)
        ttn_sf(LORAWAN_SF);
        #endif

    #if defined(USE_ABP)

        // Set static session parameters. Instead of dynamically establishing a session
        // by joining the network, precomputed session parameters are be provided.
        uint8_t appskey[sizeof(APPSKEY)];
        uint8_t nwkskey[sizeof(NWKSKEY)];
        memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
        memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
        LMIC_setSession(0x1, DEVADDR, nwkskey, appskey);

        // TTN uses SF9 for its RX2 window.
        LMIC.dn2Dr = DR_SF9;

        // Trigger a false joined
        _ttn_callback(EV_JOINED);

    #elif defined(USE_OTAA)

        // Make LMiC initialize the default channels, choose a channel, and
        // schedule the OTAA join
        LMIC_startJoining();

        #ifdef SINGLE_CHANNEL_GATEWAY
        // LMiC will already have decided to send on one of the 3 default
        // channels; ensure it uses the one we want
        LMIC.txChnl = SINGLE_CHANNEL_GATEWAY;
        #endif

        Preferences p;
        p.begin("lora", true); // we intentionally ignore failure here
        uint32_t netId = p.getUInt("netId", UINT32_MAX);
        uint32_t devAddr = p.getUInt("devAddr", UINT32_MAX);
        uint8_t nwkKey[16], artKey[16];
        bool keysgood = p.getBytes("nwkKey", nwkKey, sizeof(nwkKey)) == sizeof(nwkKey) && 
                        p.getBytes("artKey", artKey, sizeof(artKey)) == sizeof(artKey);
        p.end(); // close our prefs

        if(!keysgood) {
            // We have not yet joined a network, start a full join attempt
            // Make LMiC initialize the default channels, choose a channel, and
            // schedule the OTAA join
            Serial.println("No session saved, joining from scratch");
            LMIC_startJoining();
        }
        else {
            Serial.println("Rejoining saved session");
            screen_print("Rejoining saved session\n");
            LMIC_setSession(netId, devAddr, nwkKey, artKey);

            // Trigger a false joined
            _ttn_callback(EV_JOINED);
        }

    #endif
}

void ttn_sf(unsigned char sf) {
    LMIC_setDrTxpow(sf, 14);
}

void ttn_adr(bool enabled) {
    LMIC_setAdrMode(enabled);
    LMIC_setLinkCheckMode(!enabled);
}

uint32_t ttn_get_count() {
  return count;
}

static void ttn_set_cnt() {
    LMIC_setSeqnoUp(count);

    // We occasionally mirror our count to flash, to ensure that if we lose power we will at least start with a count that is almost correct 
    // (otherwise the TNN network will discard packets until count once again reaches the value they've seen).  We limit these writes to a max rate
    // of one write every 5 minutes.  Which should let the FLASH last for 300 years (given the ESP32 NVS algoritm)
    static uint32_t lastWriteMsec = UINT32_MAX; // Ensure we write at least once
    uint32_t now = millis();
    if(now < lastWriteMsec || (now - lastWriteMsec) > 5 * 60 * 1000L) { // write if we roll over (50 days) or 5 mins
        lastWriteMsec = now;

        Preferences p;
        if(p.begin("lora", false)) {
            p.putUInt("count", count);
            p.end();
        }
    }
}

/// Save counter - can be used on power down
void ttn_save_counter() {
    Preferences p;
    if(p.begin("lora", false)) {
        p.putUInt("count", count);
        screen_print("Frame Count saved!\n");
        p.end();
    }
}

/// Blow away our prefs (i.e. to rejoin from scratch)
void ttn_erase_prefs() {
    Preferences p;
    if(p.begin("lora", false)) {
        p.clear();
        p.end();
    }
}

void ttn_send(uint8_t * data, uint8_t data_size, uint8_t port, bool confirmed){
    ttn_set_cnt(); // we are about to send using the current packet count

    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println("* ttn_send: previous operation still in progress, aborting...");
        screen_print("RF Busy - DISCARDED");
        _ttn_callback(EV_PENDING);
        return;

         //Serial.println("* ttn_send: previous operation still in progress, discarding to send new one...");
         //LMIC_clrTxData();
    }

    // Prepare upstream data transmission at the next possible time.
    // Parameters are port, data, length, confirmed
    int retCode = LMIC_setTxData2(port, data, data_size, confirmed ? 1 : 0);
    // Serial.printf("* ttn_send: queued, return code %i", retCode);
    // Serial.println("");

    _ttn_callback(EV_QUEUED);
    count++;
}

void ttn_loop() {
    os_runloop_once();
}
