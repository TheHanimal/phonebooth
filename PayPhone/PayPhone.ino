#include <Bounce.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TimeLib.h>  //I don't know if I have this one
#include <MTP_Teensy.h>
#include "play_sd_wav.h"

// DEFINES
// Define pins used by Teensy Audio Shield
//These are hardcoded and shouldn't need changing
#define SDCARD_CS_PIN 10   //select line
#define SDCARD_MOSI_PIN 7  //data line
#define SDCARD_SCK_PIN 14  //clock line
// And those used for inputs
#define HOOK_PIN 0
#define PLAYBACK_BUTTON_PIN 1

#define noINSTRUMENT_SD_WRITE

// GLOBALS
// Audio initialisation code can be generated using the GUI interface at hhtps:://www.pjrc.com/teensy/gui/
// Inputs
AudioSynthWaveform waveform1;  // synthizer fx to create the "beep" sfx
AudioInputI2S i2s2;            // microphone input from audio shield I2S
//AudioPlaySdRaw         playRaw1; //Play .RAW audio files saved on SD card
//AudioPlaySdWav         playWav1; // Plays greeting message | Play 44.1kHz 16-bit PCM greeting WAV file
AudioPlaySdWavX playWav1;  // new code playing for greeting | play 44.1kHz 16big
// Outputs
AudioRecordQueue queue1;  // Creating an audio buffer in memory before saving to SD
AudioMixer4 mixer;        // Allows merging several inputs to same output
AudioOutputI2S i2s1;      // I2S interface to Speaker/Line Out on Audio Shield
// Connections - 'cabels' to plug inputs to outputs
AudioConnection patchCord1(waveform1, 0, mixer, 0);  // wave to mixer
//AudioConnection patchCord2 (playRaw1, 0, mixer, 1); // raw audio to mixer
AudioConnection patchCord3(playWav1, 0, mixer, 1);  // wav file playback mixer
AudioConnection patchCord4(mixer, 0, i2s1, 0);      // mixer outupt to speaker (L)
AudioConnection patchCord6(mixer, 0, i2s1, 1);      // mixer output to speaker (R)
AudioConnection patchCord5(i2s2, 0, queue1, 0);     // mic input to queue (L)
AudioControlSGTL5000 sgtl5000_1;                // audio control chip

// Filename to save audio recording to SD card
char filename[15];
// The file object itself
File frec;

// Use long 40ms debounce time on both switches
Bounce buttonRecord = Bounce(HOOK_PIN, 40);           
Bounce buttonPlay = Bounce(PLAYBACK_BUTTON_PIN, 40);  

// Keep track of current state of the device
enum Mode {Initialising, Ready, Prompting, Recording, Playing};  
Mode mode = Mode::Initialising;

float beep_volume = 0.02f;  // not too loud :)

uint32_t MTPcheckInterval;  // default value of device check interval [ms]

// variables for writing to WAV file
unsigned long ChunkSize = 0L;
unsigned long Subchunk1Size = 16;
unsigned int AudioFormat = 1;
unsigned int numChannels = 1;
unsigned long sampleRate = 44100;
unsigned int bitsPerSample = 16;
unsigned long byteRate = sampleRate * numChannels * (bitsPerSample / 8);  // samplerate x channels x (bitspersample / 8)
unsigned int blockAlign = numChannels * bitsPerSample / 8;
unsigned long Subchunk2Size = 0L;
unsigned long recByteSaved = 0L;
unsigned long NumSamples = 0L;
byte byte1, byte2, byte3, byte4;


void setup() {  // runs when power is first recieved

  // Note that Serial.begin() is not req. for Teensey -
  // by deafualt it initialises serial communication at full USB speed
  // see https://www.pjrc.com/teensy/td_serial.html
  // Serial.begin()
  //Serial.println(_FILE_ _DATE_); not used and added new code for start
  Serial.begin(9600);
  while (!Serial && millis() < 5000) {
    // wait for serial port to connect
  }
  Serial.println("Serial set up correctly");
  Serial.printf("Audio block set to %d samples/n", AUDIO_BLOCK_SAMPLES);

  // Configure the input pins
  pinMode(HOOK_PIN, INPUT_PULLUP);
  pinMode(PLAYBACK_BUTTON_PIN, INPUT_PULLUP);  //look up I_P and other modes

  // Audio connection require memory, and the record queue
  // use this memory to buffer incoming audio.
  AudioMemory(60);

  // Enable the audio shiled, select input, and enable output
  sgtl5000_1.enable();
  // Define which input on the audio shild to us (AUDIO_INPUT_LINEIN / AUDIO_INPUT_MIC)
  sgtl5000_1.inputSelect(AUDIO_INPUT_MIC);
  sgtl5000_1.volume(0.75);  // Speaker volume level -- feel free to change

  mixer.gain(0, 1.0f);
  mixer.gain(1, 1.0f);

  // Play a beep to indicate system is online
  waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
  //waveform1.frequencey(440);
  //waveform1.amplitude(0.9);
  wait(1000);
  waveform1.amplitude(0);
  delay(1000);

  // Initialize the SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  if (!(SD.begin(SDCARD_CS_PIN))) {
    // stop here if no SD card, but print a message
    while (1) {
      Serial.println("Unable to access the SD card");
      delay(500);
    }
  } else Serial.println("SD card correctly initialized");

  // mandator to begin the MTP session
  MTP.begin();

  // Add SD CARD
  MTP.addFilesystem(SD, "GSL Hopeline");  // choose name to appear in file explorer
  Serial.println("Added SD card via MTP");
  MTPcheckInterval = MTP.storage()->get_DeltaDeviceCheckTimeMS();

  // Value in dB
  sgtl5000_1.micGain(11);  // set through trial and error. 36 min gain of 15dB
  // sgt15000_1.micGain(5); //new code says much lower gain is required for AOM5024 electret capsule

  // Synchornise the Time object used in the program code with the RTC time provider.
  // see https:://github.com/PaulStoffregen/Time
  setSyncProvider(getTeensy3Time);  //uses realtime clock on teensy

  // Define a callback that will assign the correct datetime for any file system operations
  // (i.e saving a new audio recordin to the SD card)
  FsDateTime::setCallback(dateTime);
 
  mode = Mode::Ready;
  print_mode();  // Done with setup
}

void loop() {
  // First, read the buttons - check the inputs
  buttonRecord.update();  // if handset has been replaced or not
  buttonPlay.update();    // in example its button on back of phone

  switch (mode) {
    case Mode::Ready:
      // Rising edge occurs when the handset is lifted
      if (buttonRecord.fallingEdge()) {  // orig. .risingEdge 41min
        Serial.println("Handset lifted");
        mode = Mode::Prompting; print_mode();
        print_mode();  // if picked up THEN play prompt
      } else if (buttonPlay.fallingEdge()) {
        //playAllRecordings();
        //playLastRecording();
        playLakeSounds();
      }
      break;

    case Mode::Prompting:
      // wait one sec for users to put handset to their ear
      wait(1500);
      // Play the greeting inviting them to record their message
      playWav1.play("greeting.wav");
      // Wait till message has finsihed playing
      //while (playWav1.isPlaying()) {
      while (!playWav1.isStopped()) {
        // Check wheter the handset is replaced
        buttonRecord.update();
        buttonPlay.update();
        // Handset is replaced
        if (buttonRecord.risingEdge()) {  // orig .fallingEdge
          playWav1.stop();
          mode = Mode::Ready;
          print_mode();
          return;
        }
        if (buttonPlay.fallingEdge()) {
          playWav1.stop();
          //playAllRecordings();
          //playLastRecording();
          playLakeSounds();
          return;
        }
      }


      // Debug message
      Serial.println("Starting Recording");
      // Play the tone sound effect
      waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
      //waveform1.amplitude(0.9);
      wait(1250);
      waveform1.amplitude(0);
      // Start recording function
      startRecording();  // see func below
      break;

    case Mode::Recording:
      // Handset is replaced
      if (buttonRecord.risingEdge()) {  // orig .fallingEdge
        // Debug log
        Serial.println("Stopping Recording");
        // Stop Recording
        stopRecording();
        // Play audio tone to confirm recording has ended
        end_Beep();
      } else {
        continueRecording();
      }
      break;

    case Mode::Playing:  // to make compiler happy
      break;

    case Mode::Initialising:  // to make compiler happy
      break;
  }

  MTP.loop();  // this is mandatory to be placed in the loop
}

// new code for setting mtp device
void setMTPdeviceChecks(bool nable) {
  if (nable) {
    MTP.storage()->set_DeltaDeviceCheckTimeMS(MTPcheckInterval);
    Serial.print("En");
  } else {
    MTP.storage()->set_DeltaDeviceCheckTimeMS((uint32_t)-1);
    Serial.print("Dis");
  }
  Serial.println("abled MTP storage device checks");
}

#if defined(INSTRUMENT_SD_WRITE)
static uint32_t worstSDwrite, printNext;
#endif  // defined(INSTRUMENT_SD_WRITE)

void startRecording() {
  setMTPdeviceChecks(false);  // disable mtp device checks while recording
#if defined(INSTRUMENT_SD_WRITE)
  worstSDwrite = 0;
  printNext = 0;
#endif  // defined(INSTURMENT_SD_WRITE)

  // Find the first available file number
  //for (uint8_t i=0; i<9999; i++) { // BUGFIX uint8_t overflows if it reaches 255
  for (uint16_t i = 0; i < 9999; i++) {
    // Format the counter as a five-digit number with leading zeros, followed by file ext
    snprintf(filename, 11, " %05d.wav", i);  // % a string of 'd' a number with 5 places | ie 00001.RAW
    // Create if does not exist, do not open existing, writ, sync after write
    if (!SD.exists(filename)) {
      break;
    }
  }
  frec = SD.open(filename, FILE_WRITE);
  Serial.println("Opened file !");
  if (frec) {
    Serial.print("Recording to ");
    Serial.print(filename);
    queue1.begin();
    mode = Mode::Recording;
    print_mode();
    recByteSaved = 0L;
  } else {
    Serial.println("Couldn't open file to record!");
  }
}

void continueRecording() {
#if defined(INSTRUMENT_SD_WRITE)
  uint32_t started = micros();
#endif
#define NBLOX 16
  // Check if there is datea in the queue
  if (queue1.available() >= NBLOX) {
    byte buffer[NBLOX*AUDIO_BLOCK_SAMPLES*sizeof(int16_t)];
    // Fetch 2 blocks from the audio library and copy
    // into a 512 byte buffer. The Arduino SD library
    // is the most efficient when full 512 byte sector size
    // writes are used.
    for (int i = 0; i < NBLOX; i++) {
      memcpy(buffer + i * AUDIO_BLOCK_SAMPLES * sizeof(int16_t), queue1.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
      queue1.freeBuffer();
    }
    // Write all 512 bytes to the SD card
    frec.write(buffer, sizeof buffer);
    recByteSaved += sizeof buffer;
  }

#if defined(INSTRUMENT_SD_WRITE)
  started = micros() - started;
  if (started > worstSDwrite)
    worstSDwrite = started;

  if (millis() >= printNext) {
    Serial.printf("Worst write took %luus\n", worstSDwrite);
    worstSDwrite = 0;
    printNext = millis() + 250;
  }
#endif
}

void stopRecording() {
  // Stop radding any new data to the queue
  queue1.end();
  // Flush all existing remaining data from the queue
  while (queue1.available() > 0) {
    // Save to open file
    frec.write((byte*)queue1.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
    queue1.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES * sizeof(int16_t);
  }
  writeOutHeader();
  // Close the file
  frec.close();
  Serial.println("Closed file");
  mode = Mode::Ready;
  print_mode();
  setMTPdeviceChecks(true);  // enable MTP device checks, recoridng is finsihed
}

void playAllRecordings() {
  // Recording files are saved in the root directory
  File dir = SD.open("/");

  while (true) {
    File entry = dir.openNextFile();
    if (strstr(entry.name(), "greeting")) {
      entry = dir.openNextFile();
    }
    if (!entry) {
      // no more files
      entry.close();
      end_Beep();
      break;
    }

    // int8_t len = strlen(entry.name());
    // if (strstr(strlwr(entry.name() + (len - 4)), ".raw")) { // explain at 55min
    if (strstr(entry.name(), ".wav") || strstr(entry.name(), ".wav")) {
      Serial.print("Now playing ");
      Serial.println(entry.name());
      // Play a short beep before each message
      waveform1.amplitude(beep_volume);  // is freq missing?
      wait(750);
      waveform1.amplitude(0);
      // Play the file
      playWav1.play(entry.name());
      mode = Mode::Playing;
      print_mode();
    }

    entry.close();

    // while (playRaw1.isPlaying()) { // works for playRaw but not playWav
    while (!playWav1.isStopped()) {
      buttonPlay.update();
      buttonRecord.update();
      // Button is pressed again
      //if(buttonPlay.risingEdge() || buttonRecord.fallingEdge()) { // MIGHT have to change
      if (buttonPlay.fallingEdge() || buttonRecord.risingEdge()) {  // might need to change to .fallingEdge
        playWav1.stop();
        mode = Mode::Ready;
        print_mode();
        return;
      }
    }
  }
  // All files have been played
  mode = Mode::Ready;
  print_mode();
}

void playLastRecording() {
  // find the first available file number
  uint16_t idx = 0;
  for (uint16_t i=0; i<9999; i++) {
    // format the counter as a five digit number with leading series followed by file extension
    snprintf(filename, 11, " %05d.wav", i);
    // check if file with index i exists
    if (!SD.exists(filename)){
      idx = i - 1;
      break;
    }
  }
  // now play file with index idx == last recorded file
  snprintf(filename, 11, " %05d.wav", idx);
  Serial.println(filename);
  playWav1.play(filename);
  mode = Mode::Playing; print_mode();
  while (!playWav1.isStopped()){ // this works for playWav
    buttonPlay.update();
    buttonRecord.update();
    // Button is pressed again
    if(buttonPlay.fallingEdge() || buttonRecord.risingEdge()) {
      playWav1.stop();
      mode = Mode::Ready; print_mode();
      return;
    }
  }
  // file has been played
  mode = Mode::Ready; print_mode();
  end_Beep();
}

void playLakeSounds() {
  const char *filename = "greeting.wav";

  if (SD.exists(filename)) {
    Serial.println("Playing greeting.wav...");
    mode = Mode::Playing; print_mode();
    playWav1.play(filename);
    while (!playWav1.isStopped()){ // this works for playWav
    buttonPlay.update();
    buttonRecord.update();
    // Button is pressed again
    if(buttonPlay.fallingEdge() || buttonRecord.risingEdge()) {
      playWav1.stop();
      mode = Mode::Ready; print_mode();
      return;
    }
  }
  // file has been played
  mode = Mode::Ready; print_mode();
  end_Beep();
  }
}

// Retrieve the current time from Teensy built-in RTC
time_t getTeensy3Time() {
  return Teensy3Clock.get();
}

// Callback to assign timestamps for file system operations
void dateTime(uint16_t* date, uint16_t* time, uint8_t* ms10) {

  // Return date using FS_DATE macro to format fields.
  *date = FS_DATE(year(), month(), day());

  // Return time using FS_TIME macro to format fields.
  *time = FS_TIME(hour(), minute(), second());

  //Retrun low time bits in units of 10ms.
  *ms10 = second() & 1 ? 100 : 0;
}

// Non-blocking delay, which pauses executiong of main program logic,
// but while still listening for input
void wait(unsigned int milliseconds) {
  elapsedMillis msec = 0;

  while (msec <= milliseconds) {
    buttonRecord.update();
    buttonPlay.update();
    if (buttonRecord.risingEdge()) Serial.println("Button (pin 0) Press");  // orig .fallingEdge
    if (buttonPlay.fallingEdge()) Serial.println("Button (pin 1) Press");
    if (buttonRecord.fallingEdge()) Serial.println("Button (pin 0) Release");  // orig .risingEdge
    if (buttonPlay.risingEdge()) Serial.println("Button (pin 1) Release");
  }
}

void writeOutHeader() {  // update WAV header with final filesize/datasize
  Subchunk2Size = recByteSaved - 42;  // because we didnt make space for header to start with
  ChunkSize = Subchunk2Size + 34;
  frec.seek(0);
  frec.write("RIFF");
  byte1 = ChunkSize & 0xff;
  byte2 = (ChunkSize >> 8) & 0xff;
  byte3 = (ChunkSize >> 16) & 0xff;
  byte4 = (ChunkSize >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  frec.write("WAVE");
  frec.write("fmt ");
  byte1 = Subchunk1Size & 0xff;
  byte2 = (Subchunk1Size >> 8) & 0xff;
  byte3 = (Subchunk1Size >> 16) & 0xff;
  byte4 = (Subchunk1Size >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  byte1 = AudioFormat & 0xff;
  byte2 = (AudioFormat >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  byte1 = numChannels & 0xff;
  byte2 = (numChannels >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  byte1 = sampleRate & 0xff;
  byte2 = (sampleRate >> 8) & 0xff;
  byte3 = (sampleRate >> 16) & 0xff;
  byte4 = (sampleRate >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  byte1 = byteRate & 0xff;
  byte2 = (byteRate >> 8) & 0xff;
  byte3 = (byteRate >> 16) & 0xff;
  byte4 = (byteRate >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  byte1 = blockAlign & 0xff;
  byte2 = (blockAlign >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  byte1 = bitsPerSample & 0xff;
  byte2 = (bitsPerSample >> 8) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write("data");
  byte1 = Subchunk2Size & 0xff;
  byte2 = (Subchunk2Size >> 8) & 0xff;
  byte3 = (Subchunk2Size >> 16) & 0xff;
  byte4 = (Subchunk2Size >> 24) & 0xff;
  frec.write(byte1);
  frec.write(byte2);
  frec.write(byte3);
  frec.write(byte4);
  frec.close();
  Serial.println("header written");
  Serial.print("Subchunk2: ");
  Serial.println(Subchunk2Size);
}

void end_Beep(void) {
  waveform1.frequency(523.25);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
  wait(250);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
  wait(250);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
  wait(250);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
}

void print_mode(void) {  // only for debugging
  Serial.print("Mode switched to: ");
  // Initialising, Ready, Prompting, Recording, Playing
  if (mode == Mode::Ready) Serial.println(" Ready");
  else if (mode == Mode::Prompting) Serial.println(" Prompting");
  else if (mode == Mode::Recording) Serial.println(" Recording");
  else if (mode == Mode::Playing) Serial.println(" Playing");
  else if (mode == Mode::Initialising) Serial.println(" Initialising");
  else Serial.println(" Undefined");
}