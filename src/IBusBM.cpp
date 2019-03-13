/*
 * Interface to the RC IBus protocol
 * 
 * Based on original work from: https://gitlab.com/timwilkinson/FlySkyIBus
 * Extended to also handle sensors/telemetry data to be sent back to the transmitter
 * This lib requires a hardware UART for communication
 * Another version using software serial is here https://github.com/Hrastovc/iBUStelemetry)
 * 
 * Explaination of sensor/ telemetry prtocol and the circuit (R + diode) to combine the two pins here: 
 * https://github.com/betaflight/betaflight/wiki/Single-wire-FlySky-(IBus)-telemetry
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public 
 * License as published by the Free Software Foundation; either  
 * version 2.1 of the License, or (at your option) any later version.
 *   
 * Created 12 March 2019 Bart Mellink
 */

#include <Arduino.h>
#include "IBusBM.h"

/*
 *   has max 10 channels in this lib (with messagelength of 0x20 there is room for 14 channels)

  Example set of bytes coming over the line for setting your servo's: 
    20 40 DB 5 DC 5 54 5 DC 5 E8 3 D0 7 D2 5 E8 3 DC 5 DC 5 DC 5 DC 5 DC 5 DC 5 DA F3
  Explanation
    Protocol length: 20
    Command code: 40 
    Channel 0: DB 5  -> value 0x5DB
    Channel 1: DC 5  -> value 0x5Dc
    Channel 2: 54 5  -> value 0x554
    Channel 3: DC 5  -> value 0x5DC
    Channel 4: E8 3  -> value 0x3E8
    Channel 5: D0 7  -> value 0x7D0
    Channel 6: D2 5  -> value 0x5D2
    Channel 7: E8 3  -> value 0x3E8
    Channel 8: DC 5  -> value 0x5DC
    Channel 9: DC 5  -> value 0x5DC
    Channel 10: DC 5 -> value 0x5DC
    Channel 11: DC 5 -> value 0x5DC
    Channel 12: DC 5 -> value 0x5DC
    Channel 13: DC 5 -> value 0x5DC
    Checksum: DA F3
 */


void IBusBM::begin(HardwareSerial& serial) {
  serial.begin(115200);
  begin((Stream&)serial, false);
}

void IBusBM::begin(HardwareSerial& serial, bool enablesensor) {
  serial.begin(115200);
  begin((Stream&)serial, enablesensor);
}

void IBusBM::begin(Stream& stream,  bool enablesensor) {
  this->stream = &stream;
  this->state = DISCARD;
  this->last = millis();
  this->ptr = 0;
  this->len = 0;
  this->chksum = 0;
  this->lchksum = 0;
  this->enablesensor = enablesensor;
}

void IBusBM::loop(void) {
  while (stream->available() > 0) {
    uint32_t now = millis();
    if (now - last >= PROTOCOL_TIMEGAP){
      state = GET_LENGTH;
    }
    last = now;
    
    uint8_t v = stream->read();
    switch (state){
      case GET_LENGTH:
        if (v <= PROTOCOL_LENGTH && v > PROTOCOL_OVERHEAD) {
          ptr = 0;
          len = v - PROTOCOL_OVERHEAD;
          chksum = 0xFFFF - v;
          state = GET_DATA;
        } else {
          cnt_err++;
          state = DISCARD;
        }
        break;

      case GET_DATA:
        buffer[ptr++] = v;
        chksum -= v;
        if (ptr == len) {
          state = GET_CHKSUML;
        }
        break;
        
      case GET_CHKSUML:
        lchksum = v;
        state = GET_CHKSUMH;
        break;

      case GET_CHKSUMH:
        // Validate checksum
        if (chksum == (v << 8) + lchksum) {
          // Checksum is all fine Execute command - 
          uint8_t adr = buffer[0] & 0x0f;
          if (buffer[0]==PROTOCOL_COMMAND40) {
            // Valid servo command received - extract channel data
            for (uint8_t i = 1; i < PROTOCOL_CHANNELS * 2 + 1; i += 2) {
              channel[i / 2] = buffer[i] | (buffer[i + 1] << 8);
            }
            cnt_rec++;
          } else if (adr<=sensorCount && adr>0 && len==1 && enablesensor) {
            // all sensor data commands go here
            // we only process the len==1 commands (=message length is 4 bytes incl overhead) to prevent the case the
            // return messages from the UART TX port loop back to the RX port and are processed again. This is extra
            // precaution as it will also be prevented by the PROTOCOL_TIMEGAP required
            delayMicroseconds(100);
            switch (buffer[0] & 0x0f0) {
              case PROTOCOL_COMMAND_DISCOVER: // 0x80, discover sensor
                cnt_poll++;  
                // echo discover command: 0x04, 0x81, 0x7A, 0xFF 
                stream->write(0x04);
                stream->write(PROTOCOL_COMMAND_DISCOVER + adr);
                chksum = 0xFFFF - (0x04 + PROTOCOL_COMMAND_DISCOVER + adr);
                break;
              case PROTOCOL_COMMAND_TYPE: // 0x90, send sensor type
                // echo sensortype command: 0x06 0x91 0x00 0x02 0x66 0xFF 
                stream->write(0x06);
                stream->write(PROTOCOL_COMMAND_TYPE + adr);
                stream->write(sensorType[adr]);
                stream->write(0x02); // always this value - unkwown
                chksum = 0xFFFF - (0x06 + PROTOCOL_COMMAND_TYPE + adr + sensorType[adr] + 2);
                break;
              case PROTOCOL_COMMAND_VALUE: // 0xA0, send sensor data
                cnt_sensor++;
                // echo sensor value command: 0x06 0x91 0x00 0x02 0x66 0xFF 
                stream->write(0x06);
                stream->write(PROTOCOL_COMMAND_VALUE + adr);
                stream->write(sensorValue[adr] & 0x0ff);
                stream->write(sensorValue[adr] >> 8); 
                chksum = 0xFFFF - (0x06 + PROTOCOL_COMMAND_VALUE + adr + (sensorValue[adr]>>8) + (sensorValue[adr]&0x0ff));
                break;
              default:
                adr=0; // unknown command, prevent sending chksum
                break;
            }
            if (adr>0) {
              stream->write(chksum & 0x0ff);
              stream->write(chksum >> 8);              
            }
          }
        }
        state = DISCARD;
        break;

      case DISCARD:
      default:
        break;
    }
  }
}

uint16_t IBusBM::readChannel(uint8_t channelNr) {
  if (channelNr < PROTOCOL_CHANNELS) {
    return channel[channelNr];
  } else {
    return 0;
  }
}

uint8_t IBusBM::addSensor(uint8_t type) {
  // add a sensor, return sensor number
  if (sensorCount < SENSORMAX) {
    sensorCount++;
    sensorType[sensorCount] = type;
  }
  return sensorCount;
}

void IBusBM::setSensorMeasurement(uint8_t adr, uint16_t value){
   if (adr<=sensorCount)
     sensorValue[adr] = value;
}