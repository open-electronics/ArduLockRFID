/*
 *    ArduLock RFID
 *    Author: Luca Bellan
 */

#include <EEPROMex.h>               //  https://github.com/thijse/Arduino-EEPROMEx
#include <Wire.h>
#include <RTClib.h>                 //  https://github.com/adafruit/RTClib
#include <Keypad.h>                 //  https://playground.arduino.cc/Code/Keypad/#Download
#include <PN532_I2C.h>              //  https://github.com/elechouse/PN532        
#include <PN532.h>                  //  https://github.com/elechouse/PN532  
#include <NfcAdapter.h>             //  https://github.com/elechouse/PN532  
#include <LiquidCrystal_I2C.h>      //  https://github.com/fdebrabander/Arduino-LiquidCrystal-I2C-library
#include <SPI.h>
#include <SD.h>


//  Configuration
#define MASTER_KEY 123456           //  For enter in Settings
#define DEBUG_ON                  //  Uncomment for serial debug
#define BUZZER_ON                   //  Uncomment for buzzer sounds
#define RESET_TIME 20               //  Time (sec) elapsed after last key press -> return to Menu
#define CONFIRM_KEY '#'             //  Confirmation key (enter) in the keyboard


//  Definitions
#ifdef DEBUG_ON
  bool Debug = true;
#else
  bool Debug = false;
#endif
#define PIN_BUZZER      6
#define PIN_RELAY       13
#define PIN_SD_CS       53
#define KEY_ROWS        4
#define KEY_COLUMNS     4
byte RowPins[KEY_ROWS] = {2,3,4,5};
byte ColPins[KEY_COLUMNS] = {7,8,9,10};
char Keys[KEY_ROWS][KEY_COLUMNS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
char ConfirmKey = CONFIRM_KEY;
typedef struct {
  long Code[10];
  uint32_t RFIDCode[10];
  byte HHStartActive;
  byte HHEndActive;
  byte LockBlockAttempts;
  byte LockBlockSeconds;
  byte RelayActiveSeconds;
} Configuration;


//  Variables
PN532_I2C pn532i2c(Wire);
PN532 nfc(pn532i2c);
RTC_DS1307 rtc;
Keypad keypad = Keypad(makeKeymap(Keys), RowPins, ColPins, KEY_ROWS, KEY_COLUMNS);
LiquidCrystal_I2C lcd(0x27, 16, 2);
char KeyBuffer[10];
byte KeyBufferIndex = 0;
byte CurrentPage = 0;
long LastKeyPressTime = 0;
long BlockTime = 0;
int AttemptsCount = 0;
bool AttemptBlocked = false;
Configuration Config;
bool NFCSuccess = false;
uint8_t NFCuid[] = { 0, 0, 0, 0, 0, 0, 0 };
uint8_t NFCuidLength = 0;
long LastRFIDReadTime;
long Val;
String StringBuffer;
long LCDDisplayTime;
long LCDRefreshTime;
bool SDInitialized;
  

void setup() {

  //  System init
  if(Debug) Serial.begin(115200);

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  if (!SD.begin(PIN_SD_CS)) {
    SDInitialized = false;
    WriteLog("Error: no SD card");
  } else {
    SDInitialized = true;
    WriteLog("SD card initialized.");
  }
  
  Wire.begin();
  rtc.begin();
  if (!rtc.begin()) {
    WriteLog("Didn't find RTC: halt");
    while (1);
  }
  if (!rtc.isrunning()) {
    WriteLog("RTC is NOT running: time adjusted.");
    rtc.adjust(DateTime(__DATE__, __TIME__));
  }

  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  while (!versiondata) {
    WriteLog("Didn't find PN53x: halt.");
    versiondata = nfc.getFirmwareVersion();
    delay(100);
  }
  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();

  lcd.init();
  lcd.backlight();
  lcd.clear();

  //  Pins init
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  
  //  Check for reading or initializing EEPROM
  if(EEPROM.read(1) == 255) {
    //  Fist EEPROM init
    for (int i=0; i<10; i++) {
      Config.Code[i] = 0;
    }
    for (int i=0; i<10; i++) {
      Config.RFIDCode[i] = 0;
    }
    Config.HHStartActive = 0;
    Config.HHEndActive = 23;
    Config.LockBlockAttempts = 3;
    Config.LockBlockSeconds = 10;
    Config.RelayActiveSeconds = 1;
    EEPROM.writeBlock<Configuration>(1, Config);
  } else {
    //  Read saved values from EEPROM
    EEPROM.readBlock<Configuration>(1, Config);
  }

  WriteLog("EEPROM CONFIGURATION:");
  WriteLog("Codes:");
  for (int i=0; i<10; i++) {
    WriteLog(String(Config.Code[i]));
  }
  WriteLog("RFIDs associated:");
  for (int i=0; i<9; i++) {
    WriteLog(String(Config.RFIDCode[i]));
  }
  WriteLog("Start lock active hour: " + String(Config.HHStartActive));
  WriteLog("End lock active hour: " + String(Config.HHEndActive));
  WriteLog("Lock block attempts: " + String(Config.LockBlockAttempts));
  WriteLog("Lock block seconds: " + String(Config.LockBlockSeconds));
  WriteLog("Relay active seconds: " + String(Config.RelayActiveSeconds));
  WriteLog("- - - - - - - - - - - - - - - - - -");
  
}


void loop() {

  ReadKeys();
  ReadRFID();
  CheckResetTime();
  CheckBlockedTime();
  RefreshLCD();

}


//  Read keys pressed from keyboard
void ReadKeys() {

  char Key = keypad.getKey();
  if (Key != NO_KEY) {
    Buzz(100, 0);
    DateTime CurrentDate = rtc.now();
    LastKeyPressTime = CurrentDate.secondstime();
    if(Key - '0' >= 0 && Key - '0' < 10 && KeyBufferIndex < 8) {
      KeyBuffer[KeyBufferIndex] = Key;
      KeyBufferIndex++;
    } else if(Key == ConfirmKey) {
      WriteLog("Entered: " + String(KeyBuffer));
      ManageActions(true);
      ResetKeyBuffer();
    }    
  }
  
}


//  After Confirm key is pressed: manage actions by the current page
void ManageActions(bool confirm) {

  if(confirm == true) {
    DateTime CurrentDate = rtc.now();
    switch(CurrentPage) {
      //  Menu: lock key or master key entered
      case 0:
        if(AttemptBlocked == false && String(KeyBuffer) == String(MASTER_KEY)) {
          //  Master key
          AttemptsCount = 0;
          WriteLCD("SETTINGS", "Digit 1-11", 0);
          WriteLog("Master key OK: enter in Settings.");
          CurrentPage = 1;
          Buzz(50, 2);
        } else if(AttemptBlocked == false && IsGood() && CurrentDate.hour() >= Config.HHStartActive && CurrentDate.hour() <= Config.HHEndActive) {
          //  Lock key
          WriteLCD("CODE OK", "Lock open", 3);
          AttemptsCount = 0;
          WriteLog("Lock key OK: open lock.");
          digitalWrite(PIN_RELAY, HIGH);
          ReturnToMenu(true);
          delay((Config.RelayActiveSeconds - 1) * 1000);
          digitalWrite(PIN_RELAY, LOW);
        } else {
          WriteLCD("ERROR", "Invalid code", 3);
          WriteLog("Master key or Lock key not valid.");
          if(Config.LockBlockAttempts != 0) {
            AttemptsCount++;
            if(AttemptBlocked == false && AttemptsCount > Config.LockBlockAttempts) {
              WriteLCD("ERROR", "Lock blocked", 3);
              WriteLog("Max attempts reached, lock is blocked for " + String((Config.LockBlockSeconds)) + " sec.");
              BlockTime = CurrentDate.secondstime();
              AttemptBlocked = true;
            }
          }
          ReturnToMenu(false);
        }
      break;
      //  Settings (main)
      case 1:
        ManageSettings();
      break;
      //  Settings: code add
      case 2:
        if(AddCode(atol(KeyBuffer))) {
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("ADD CODE", "Code addedd", 3);
          WriteLog("Code addedd.");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong code", 3);
          WriteLog("Code not addedd, wrong value.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: code remove
      case 3:
        if(RemoveCode(atol(KeyBuffer))) {
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("REMOVE CODE", "Code removed", 3);
          WriteLog("Code removed.");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong value", 3);
          WriteLog("Code not removed, wrong value.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: RFID association
      case 4:
        WriteLCD("ERROR", "No RFID read", 3);
        WriteLog("No RFID card read, return to menu.");
        ReturnToMenu(false);
      break;
      //  Settings: RFID remove
      case 5:
        Val = atol(KeyBuffer);
        if(Val >= 1 && Val <= 10) {
          Config.RFIDCode[Val-1] = 0;
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("REMOVE RFID", "RFID removed", 3);
          WriteLog("Removed RFID in Position " + String(Val) + ": " + String(Config.RFIDCode[Val-1]) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "No RFID removed", 3);
          WriteLog("Error, no RFID removed.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set start active hour
      case 6:
        Val = atol(KeyBuffer);
        if(Val >= 0 && Val <= 23) {
          Config.HHStartActive = Val;
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("START ACTIVE HH", "Start HH set", 3);
          WriteLog("Start active hour set to: " + String(Val) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Start HH wrong", 3);
          WriteLog("Start active hour wrong.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set end active hour
      case 7:
        Val = atol(KeyBuffer);
        if(Val >= 0 && Val <= 23) {
          Config.HHEndActive = Val;
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("END ACTIVE HH", "End HH set", 3);
          WriteLog("End active hour set to: " + String(Val) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "End HH wrong", 3);
          WriteLog("End active hour wrong.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set manual date (GGMMAAAA)
      case 8:
        StringBuffer = String(KeyBuffer);
        if(StringBuffer.length() == 8) {
          DateTime CurrentDate = rtc.now();
          rtc.adjust(DateTime(StringBuffer.substring(4, 8).toInt(), StringBuffer.substring(2, 4).toInt(), StringBuffer.substring(0, 2).toInt(), CurrentDate.hour(), CurrentDate.minute(), CurrentDate.second() ));
          CurrentDate = rtc.now();
          WriteLCD("SET DATE", "Date set", 3);
          WriteLog("Date set: " + String(CurrentDate.day()) + "/" + String(CurrentDate.month())  + "/" + String(CurrentDate.year()) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong date", 3);
          WriteLog("Date not set, wrong value: " + StringBuffer + ".");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set manual time (HHMMSS)
      case 9:
        StringBuffer = String(KeyBuffer);
        if(StringBuffer.length() == 6) {
          DateTime CurrentDate = rtc.now();
          rtc.adjust(DateTime(CurrentDate.year(), CurrentDate.month(), CurrentDate.day(), StringBuffer.substring(0, 2).toInt(), StringBuffer.substring(2, 4).toInt(), StringBuffer.substring(4, 6).toInt()  ));
          CurrentDate = rtc.now();
          WriteLCD("SET TIME", "Time set", 3);
          WriteLog("Time set: " + String(CurrentDate.hour()) + ":" + String(CurrentDate.minute())  + ":" + String(CurrentDate.second()) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong time", 3);
          WriteLog("Time not set, wrong value: " + StringBuffer + ".");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set max attempts
      case 10:
        Val = atol(KeyBuffer);
        if(Val >= 0 && Val <= 20) {
          Config.LockBlockAttempts = Val;
          AttemptsCount = 0;
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("MAX ATTEMPTS", "Max attempts set", 3);
          WriteLog("Max attempts set to: " + String(Val) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong value", 3);
          WriteLog("Max attempts wrong.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set block seconds
      case 11:
        Val = atol(KeyBuffer);
        if(Val >= 1 && Val <= 120) {
          Config.LockBlockSeconds = Val;
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("LOCK BLOCK SEC.", "Set block sec.", 3);
          WriteLog("Block seconds set to: " + String(Val) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong value", 3);
          WriteLog("Block seconds wrong.");
          ReturnToMenu(false);
        }
      break;
      //  Settings: set relay active seconds
      case 12:
        Val = atol(KeyBuffer);
        if(Val >= 1 && Val <= 30) {
          Config.RelayActiveSeconds = Val;
          EEPROM.updateBlock<Configuration>(1, Config);
          WriteLCD("RELAY ACTIVE SEC", "Set rel act sec", 3);
          WriteLog("Relay active seconds set to: " + String(Val) + ".");
          ReturnToMenu(true);
        } else {
          WriteLCD("ERROR", "Wrong value", 3);
          WriteLog("Relay active seconds wrong.");
          ReturnToMenu(false);
        }
      break;
      //  Invalid page
      default:
        ReturnToMenu(false);  
      break;
    }
  }
  
}



//  Manage settings menu navigation
void ManageSettings() {

  switch(atol(KeyBuffer)) {
    //  Enter in code add
    case 1:
      WriteLCD("ADD CODE", "6 digits", 0);
      CurrentPage = 2;
      Buzz(50, 2);
      WriteLog("Settings: Enter in code add.");
    break;
    //  Enter in code remove
    case 2:
      WriteLCD("REMOVE CODE", "6 digits", 0);
      CurrentPage = 3;
      Buzz(50, 2);
      WriteLog("Settings: Enter in code remove.");
    break;
    //  Enter in RFID association
    case 3:
      WriteLCD("ADD RFID", "1-10 & pass RFID", 0);
      CurrentPage = 4;
      Buzz(50, 2);
      WriteLog("Settings: Enter in RFID association.");
    break;
    //  Enter in RFID remove
    case 4:
      WriteLCD("REMOVE RFID", "1-10", 0);
      CurrentPage = 5;
      Buzz(50, 2);
      WriteLog("Settings: Enter in RFID remove.");
    break;
    //  Enter in start active hour
    case 5:
      WriteLCD("START ACTIVE HH", "Enter HH (0-23)", 0);
      CurrentPage = 6;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set start active hour.");
    break;
    //  Enter in end active hour
    case 6:
      WriteLCD("END ACTIVE HH", "Enter HH (0-23)", 0);
      CurrentPage = 7;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set end active hour.");
    break;
    //  Enter in set manual date (GGMMAAAA)
    case 7:
      WriteLCD("SET DATE", "Format DDMMYYYY", 0);
      CurrentPage = 8;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set manual date GGMMAAAA.");
    break;
    //  Enter in set manual time (HHMMSS)
    case 8:
      WriteLCD("SET TIME", "Format HHMMSS", 0);
      CurrentPage = 9;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set manual time HHMMSS.");
    break;
    //  Enter in set max attempts
    case 9:
      WriteLCD("MAX ATTEMPTS", "0-20,0=no active", 0);
      CurrentPage = 10;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set max attempts.");
    break;
    //  Enter in set block seconds
    case 10:
      WriteLCD("LOCK BLOCK SEC.", "1-120", 0);
      CurrentPage = 11;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set block seconds.");
    break;
    //  Enter in set relay active seconds
    case 11:
      WriteLCD("RELAY ACTIVE SEC", "1-30", 0);
      CurrentPage = 12;
      Buzz(50, 2);
      WriteLog("Settings: Enter in set relay active seconds.");
    break;
    //  Invalid entry
    default:
      WriteLCD("ERROR", "Invalid entry", 3);
      ReturnToMenu(false);
    break;
  }
  
}


//  Read RFID card
void ReadRFID() {

  NFCSuccess = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &NFCuid[0], &NFCuidLength, 15);
  if(NFCSuccess == true){
    DateTime CurrentDate = rtc.now();
    bool ReadSuccess = false;
    uint32_t Read = NFCuid[0];
    for (int i=1; i < NFCuidLength; i++) {
      Read = Read << 8;
      Read |= NFCuid[i];
    }
    WriteLog("Read RFID: " + String(Read));
    if(CurrentPage == 0 && CurrentDate.secondstime() >= LastRFIDReadTime + 3 && AttemptBlocked == false && CurrentDate.hour() >= Config.HHStartActive && CurrentDate.hour() <= Config.HHEndActive) {
      //  RFID open lock
      for(int i=0; i<9; i++) {
        if(Config.RFIDCode[i] == Read) {
          ReadSuccess = true;
        }
      }
      if(ReadSuccess == true) {
        AttemptsCount = 0;
        WriteLCD("RFID OK", "Lock open", 3);
        WriteLog("RFID OK: open lock.");
        digitalWrite(PIN_RELAY, HIGH);
        delay(Config.RelayActiveSeconds * 1000);
        digitalWrite(PIN_RELAY, LOW);
      } else {
        WriteLCD("ERROR", "Invalid RFID", 3);
        WriteLog("Error: invalid RFID.");
      }
    } else if(CurrentPage == 4 && CurrentDate.secondstime() >= LastRFIDReadTime + 3 ) {
      //  RFID association
      Val = atol(KeyBuffer);
      if(Val != 0 && Val >= 1 && Val <= 10) {
        Config.RFIDCode[Val-1] = Read;
        ReadSuccess = true;
        EEPROM.updateBlock<Configuration>(1, Config);
        WriteLCD("ADD RFID", "RFID associated", 3);
        WriteLog("Associated RFID in pos " + String(Val) + ": " + String(Read));
      } else {
        ReadSuccess = false;
        WriteLCD("ERROR", "Error on assoc", 3);
        WriteLog("Error on association RFID in pos " + String(Val) + ": " + String(Read));
      }
    } else {
      //  Error
      ReadSuccess = false;
    }
    LastRFIDReadTime = CurrentDate.secondstime();
    ReturnToMenu(ReadSuccess);
  }

}


//  Check if last key was pressed too much seconds ago and reset to menu
void CheckResetTime() {

  if(CurrentPage != 0) {
    DateTime CurrentDate = rtc.now();
    if (CurrentDate.secondstime() - LastKeyPressTime > RESET_TIME) {
      WriteLog("No key pressed: return to menu.");
      ReturnToMenu(false);
    }
  }
  
}


//  Return to menu page with success or not sound
void ReturnToMenu(bool success) {

  CurrentPage = 0;
  if(success == true) {
    Buzz(700, 0);
  } else {
    Buzz(50, 5);
  }
  
}


//  Check if lock is blocked for max attempts and, eventually, release lock
void CheckBlockedTime() {

  if(AttemptBlocked == true) {
    DateTime CurrentDate = rtc.now();
    if (CurrentDate.secondstime() - BlockTime > Config.LockBlockSeconds) {
      AttemptsCount = 0;
      BlockTime = 0;
      AttemptBlocked = false;
      WriteLog("Block time elapsed, reset attempts: keyboard is free.");
    }
  }
  
}


//  Manage buzzer sound
void Buzz(int interval, byte repeat) {
  
  #ifdef BUZZER_ON
    if (repeat == 0) {
      tone(PIN_BUZZER, 500);
      delay(interval);
      noTone(PIN_BUZZER);
    } else {
      for (byte i=0;i<repeat;i++) {
        tone(PIN_BUZZER, 500);
        delay(interval);
        noTone(PIN_BUZZER);
        delay(interval);
      }
    }
  #endif 
  
}


//  Reset key buffer and index
void ResetKeyBuffer() {

  KeyBufferIndex = 0;
  for(byte i=0;i<10;i++) {KeyBuffer[i] = '\0';}
  
}


//  Add code to list
bool AddCode(long code) {

  if(code != 0 && code > 99999 && code < 1000000) {
    int Pos = -1;
    for (int i=0; i<10; i++) {
      if(Config.Code[i] == 0) {
        Pos = i;
        break;
      }
    }
    if(Pos == -1) {
      Pos = 9;
      for (int i=1; i<10; i++) {
        Config.Code[i-1] = Config.Code[i];
      }
    }
    Config.Code[Pos] = code;
    return true;
  } else {
    return false;
  }
  
}


//  Remove code from list
bool RemoveCode(long code) {

  if(code != 0 && code > 99999 && code < 1000000) {
    int Pos = -1;
    for (int i=0; i<10; i++) {
      if(Config.Code[i] == code) {
        Pos = i;
        break;
      }
    }
    if(Pos != -1) {
      Config.Code[Pos] = 0;
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
  
}


//  Check code validity
bool IsGood() {
  
  unsigned long key = atol(KeyBuffer);
  if (key < 100000 || key > 999999 || key == 0)
    return false;
  for (int i = 0; i < 10; i++) {
    if (key == Config.Code[i]) {
      return true;
    }
  }
  return false;

}


//  Write something on LCD display
void WriteLCD(String row1, String row2, int sec) {

  if(sec != 0) {
    DateTime CurrentDate = rtc.now();
    LCDDisplayTime = CurrentDate.secondstime() + sec;
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(row1);
  lcd.setCursor(0, 1);
  lcd.print(row2);
  
}


//  Display current date/time or clear LCDDisplayTime
void RefreshLCD() {

  DateTime CurrentDate = rtc.now();
  if(CurrentPage == 0 && CurrentDate.secondstime() >= LCDDisplayTime && CurrentDate.secondstime() >= LCDRefreshTime) {
    LCDRefreshTime = CurrentDate.secondstime() + 10;
    char d[15];
    sprintf(d, "   %02d/%02d/%02d", CurrentDate.day(), CurrentDate.month(), CurrentDate.year());
    char t[10];
    sprintf(t, "     %02d:%02d",  CurrentDate.hour(), CurrentDate.minute());
    WriteLCD(d, t, 0);
  }
  
}


//  Write line to log file and debug if active
void WriteLog(String line) {

  DateTime CurrentDate = rtc.now();
  char dt[20];
  sprintf(dt, "%02d/%02d/%02d %02d:%02d:%02d", CurrentDate.day(), CurrentDate.month(), CurrentDate.year(), CurrentDate.hour(), CurrentDate.minute(), CurrentDate.second());
  String LogLine = String(dt) + "  " + line;
  
  if(SDInitialized) {
    File LogFile = SD.open("log.txt", FILE_WRITE);
    if(LogFile) {
      LogFile.println(LogLine);
      LogFile.close();
    }
  }

  if(Debug) {
    Serial.println(LogLine);
  }
  
}

