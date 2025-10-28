#include <MIDIUSB.h>
/*
micro.build.pid=0x8039
micro.build.usb_product="ASW_M-CVdin"
*/

// ------------------ 핀 및 기타 설정 ------------------
const int gatePin1    = 5;   // 채널 1 게이트 핀 (D1)
const int clockPin1   = 6;   // 채널 1 클럭 핀 (C1)
const int gatePin2    = 7;   // 채널 2 게이트 핀 (D2)
const int clockPin2   = 8;   // 채널 2 클럭 핀 (C2)

const int clockLedPin = 18;  // 클럭 LED 핀
const int gateLedPin  = 19;  // 채널 1 게이트 LED 핀
const int gateLedPin2 = 20;  // 채널 2 게이트 LED 핀

const int pulseWidth    = 10; // 펄스 폭 (ms)
const int pulsePerBlink = 4;  // (필요에 따라 사용)

// 하드웨어 상태 변수
bool gateState1 = false;
bool gateState2 = false;
int midiClockCounter = 0;
int pulseCounter = 0;
unsigned long lastClockTime = 0;

// LED 및 노트 관련 제어 변수
int noteCounter = 0;
#define NOTE_COUNTER_THRESHOLD 16  // 임계치

// 1초(1000ms) 동안 노트 입력 없으면 카운트 리셋
unsigned long lastNoteTime    = 0;
const unsigned long resetThreshold = 1000; // 1초 후 리셋

// ------------------ USB MIDI 처리 ------------------
void readMIDI() {
  midiEventPacket_t rx;
  while ((rx = MidiUSB.read()).header != 0) {
    // 실시간 메시지 처리 (Clock 등)
    if (rx.byte1 >= 0xF8) {
      Serial1.write(rx.byte1);
    } else {
      byte status = rx.byte1;
      byte channel = status & 0x0F;
      // 채널 15(인덱스 15) 제외하고 전송
      if (channel != 15) {
        Serial1.write(rx.byte1);
        Serial1.write(rx.byte2);
        Serial1.write(rx.byte3);
      }
    }
    // CV/Gate 제어: 채널 15의 Note On/Off 처리
    byte command = rx.byte1 & 0xF0;
    byte channel = rx.byte1 & 0x0F;
    byte note = rx.byte2;
    byte velocity = rx.byte3;
    if (command == 0x90 && velocity > 0) { // Note On
      if (channel == 15) {
        if (note == 36) {
          handleNoteOn1(note);
        } else if (note == 38) {
          handleNoteOnGate1();
        } else if (note == 48) {
          handleNoteOn2(note);
        } else if (note == 50) {
          handleNoteOnGate2();
        }
      }
    } else if (command == 0x80 || (command == 0x90 && velocity == 0)) { // Note Off
      if (channel == 15) {
        if (note == 38) {
          handleNoteOffGate1();
        } else if (note == 50) {
          handleNoteOffGate2();
        }
      }
    }
  }
}

// ------------------ DIN MIDI (USB ↔ DIN) 파싱 관련 변수 ------------------
const uint16_t DIN_SYSEX_BUFFER_SIZE = 256;
uint8_t dinSysExBuffer[DIN_SYSEX_BUFFER_SIZE];
uint16_t dinSysExIndex = 0;
bool inSysExDIN = false;

uint8_t dinMidiBuffer[3] = {0, 0, 0};
uint8_t dinByteIndex = 0;
uint8_t dinExpectedBytes = 0;
uint8_t dinRunningStatus = 0;

// USB MIDI 패킷 전송 함수
void sendUSBMIDI(uint8_t cin, uint8_t d0, uint8_t d1, uint8_t d2) {
  midiEventPacket_t packet = { cin, d0, d1, d2 };
  MidiUSB.sendMIDI(packet);
  MidiUSB.flush();
}

// SysEx 메시지 전송 함수
void processSysExDIN() {
  uint16_t total = dinSysExIndex;
  uint16_t idx   = 0;
  while (idx < total) {
    uint8_t data[3] = {0, 0, 0};
    uint8_t count = 0;
    for (uint8_t k = 0; k < 3; k++) {
      if (idx < total) {
        data[k] = dinSysExBuffer[idx++];
        count++;
      } else {
        data[k] = 0;
      }
    }
    uint8_t cin;
    if (idx < total) {
      cin = 0x4; // SysEx 중간 패킷
    } else {
      if (count == 1)
        cin = 0x5; // 1바이트 종료 패킷
      else if (count == 2)
        cin = 0x6; // 2바이트 종료 패킷
      else
        cin = 0x7; // 3바이트 종료 패킷
    }
    sendUSBMIDI(cin, data[0], data[1], data[2]);
  }
}

// DIN MIDI 입력 읽기
void readDINMIDI() {
  while (Serial1.available() > 0) {
    uint8_t inByte = Serial1.read();
    processDINByte(inByte);
  }
}

// DIN MIDI 메시지 파싱 함수
void processDINByte(uint8_t inByte) {
  // (1) 실시간 메시지 처리
  if (inByte >= 0xF8 && inByte != 0xF7) {
    sendUSBMIDI(0xF, inByte, 0, 0);
    return;
  }
  
  // (2) SysEx 모드 처리
  if (inSysExDIN) {
    if (dinSysExIndex < DIN_SYSEX_BUFFER_SIZE)
      dinSysExBuffer[dinSysExIndex++] = inByte;
    if (inByte == 0xF7) {  // SysEx 종료
      processSysExDIN();
      dinSysExIndex = 0;
      inSysExDIN = false;
    }
    return;
  }
  
  // (3) SysEx 시작
  if (inByte == 0xF0) {
    inSysExDIN = true;
    dinSysExIndex = 0;
    dinSysExBuffer[dinSysExIndex++] = inByte;
    return;
  }
  
  // (4) 단독 F7는 무시
  if (inByte == 0xF7) {
    return;
  }
  
  // (5) 상태 바이트 처리 (일반 메시지)
  if (inByte & 0x80) {
    if (inByte >= 0xF0) {
      // 시스템 커먼 메시지 처리
      dinRunningStatus = 0;
      dinMidiBuffer[0] = inByte;
      dinByteIndex = 1;
      if (inByte == 0xF1 || inByte == 0xF3)
        dinExpectedBytes = 1;
      else if (inByte == 0xF2)
        dinExpectedBytes = 2;
      else if (inByte == 0xF6)
        dinExpectedBytes = 0;
      else
        dinExpectedBytes = 0;
      
      if (dinExpectedBytes == 0) {
        uint8_t cin = (inByte == 0xF6) ? 0x5 : 0xF;
        sendUSBMIDI(cin, dinMidiBuffer[0], 0, 0);
        dinByteIndex = 0;
      }
      return;
    } else {
      // 채널 메시지: Running Status 갱신
      dinRunningStatus = inByte;
      dinMidiBuffer[0] = inByte;
      dinByteIndex = 1;
      if ((inByte & 0xF0) == 0xC0 || (inByte & 0xF0) == 0xD0)
        dinExpectedBytes = 1;
      else
        dinExpectedBytes = 2;
      return;
    }
  }
  else {
    // (6) 데이터 바이트 처리
    if (dinByteIndex == 0 && dinRunningStatus != 0) {
      dinMidiBuffer[0] = dinRunningStatus;
      dinByteIndex = 1;
      if ((dinRunningStatus & 0xF0) == 0xC0 || (dinRunningStatus & 0xF0) == 0xD0)
        dinExpectedBytes = 1;
      else
        dinExpectedBytes = 2;
    }
    if (dinByteIndex > 0 && dinByteIndex <= dinExpectedBytes) {
      dinMidiBuffer[dinByteIndex] = inByte;
      dinByteIndex++;
    }
  }
  
  // (7) 메시지 완성 시 USB로 전송
  if (dinByteIndex == dinExpectedBytes + 1) {
    uint8_t status = dinMidiBuffer[0];
    uint8_t cin = 0;
    if (status < 0xF0) {
      switch (status & 0xF0) {
        case 0x80: cin = 0x8; break;
        case 0x90: cin = 0x9; break;
        case 0xA0: cin = 0xA; break;
        case 0xB0: cin = 0xB; break;
        case 0xC0: cin = 0xC; break;
        case 0xD0: cin = 0xD; break;
        case 0xE0: cin = 0xE; break;
        default:   cin = 0; break;
      }
    } else {
      switch (status) {
        case 0xF1: cin = 0x2; break;
        case 0xF2: cin = 0x3; break;
        case 0xF3: cin = 0x2; break;
        default:   cin = 0; break;
      }
    }
    sendUSBMIDI(cin, dinMidiBuffer[0], dinMidiBuffer[1], dinMidiBuffer[2]);
    dinByteIndex = 0;
  }
}

// ------------------ LED / CV / Gate 제어 ------------------
void handleNoteOn1(byte note) {
  digitalWrite(clockPin1, HIGH);
  lastClockTime = millis();
  // 노트 입력 시 현재 시간 기록 (리셋 타이머 역할)
  lastNoteTime = millis();
  
  noteCounter++;
  if (noteCounter % 4 == 1) {
    digitalWrite(clockLedPin, HIGH);
  }
  // noteCounter 임계치 도달 시 모듈러 연산으로 리셋
  if (noteCounter >= NOTE_COUNTER_THRESHOLD) {
    noteCounter = noteCounter % 4;
  }
}

void handleNoteOnGate1() {
  gateState1 = true;
  digitalWrite(gatePin1, HIGH);
  digitalWrite(gateLedPin, HIGH);
}

void handleNoteOn2(byte note) {
  digitalWrite(clockPin2, HIGH);
  lastClockTime = millis();
  // 노트 입력 시 현재 시간 기록
  lastNoteTime = millis();
  
  noteCounter++;
  if (noteCounter % 4 == 1) {
    digitalWrite(clockLedPin, HIGH);
  }
  if (noteCounter >= NOTE_COUNTER_THRESHOLD) {
    noteCounter = noteCounter % 4;
  }
}

void handleNoteOnGate2() {
  gateState2 = true;
  digitalWrite(gatePin2, HIGH);
  digitalWrite(gateLedPin2, HIGH);
}

void handleNoteOffGate1() {
  gateState1 = false;
  digitalWrite(gatePin1, LOW);
  digitalWrite(gateLedPin, LOW);
}

void handleNoteOffGate2() {
  gateState2 = false;
  digitalWrite(gatePin2, LOW);
  digitalWrite(gateLedPin2, LOW);
}

// LED 및 클럭 업데이트 – LED/핀을 일정 시간 후 오프, 그리고 
// 일정 시간(1초) 동안 노트 입력이 없으면 noteCounter 리셋
void updateClock() {
  if (digitalRead(clockPin1) == HIGH && millis() - lastClockTime > pulseWidth) {
    digitalWrite(clockPin1, LOW);
  }
  if (digitalRead(clockPin2) == HIGH && millis() - lastClockTime > pulseWidth) {
    digitalWrite(clockPin2, LOW);
  }
  if (digitalRead(clockLedPin) == HIGH && millis() - lastClockTime > 60) {
    digitalWrite(clockLedPin, LOW);
  }
  
  // 노트 입력이 없으면 1초 후에 noteCounter 리셋
  if (millis() - lastNoteTime > resetThreshold) {
    noteCounter = 0;
    lastNoteTime = millis(); // 리셋 시각 갱신
  }
}

// ------------------ Setup & Loop ------------------
void setup() {
  pinMode(gatePin1, OUTPUT);
  pinMode(clockPin1, OUTPUT);
  pinMode(gatePin2, OUTPUT);
  pinMode(clockPin2, OUTPUT);
  pinMode(clockLedPin, OUTPUT);
  pinMode(gateLedPin, OUTPUT);
  pinMode(gateLedPin2, OUTPUT);
  
  digitalWrite(gatePin1, LOW);
  digitalWrite(clockPin1, LOW);
  digitalWrite(gatePin2, LOW);
  digitalWrite(clockPin2, LOW);
  digitalWrite(clockLedPin, LOW);
  digitalWrite(gateLedPin, LOW);
  digitalWrite(gateLedPin2, LOW);
  
  Serial1.begin(31250);
  // 필요 시 USB 디버깅용 Serial.begin(115200);
}

void loop() {
  readMIDI();    // USB MIDI 메시지 읽어서 DIN MIDI 전송 및 CV/Gate 제어
  readDINMIDI(); // DIN MIDI 입력 처리 및 USB 전송
  updateClock(); // 클럭 및 LED 업데이트, noteCounter 리셋 처리
}