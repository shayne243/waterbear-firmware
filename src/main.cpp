#include <Arduino.h>
#include <libmaple/libmaple.h>
#include <libmaple/pwr.h> // necessary?
#include <string.h>
#include <Ezo_i2c.h>

#include "datalogger.h"
#include "scratch/dbgmcu.h"
#include "system/watchdog.h"
#include "sensors/atlas_rgb.h"

#define CH4sens PC0 //CH4 sensor Vout
#define CH4ref PC1 //CH4 sensor Vref
#define Vb PB1 //Battery voltag

int CH4s = 0;
int CH4r = 0;
int Vbat = 0;
float CH4smV = 0;
float CH4rmV = 0;
float VbatmV = 0;
float minS = 50000;
float minR = 50000;
float minV = 50000;
float mV = 5000;
float steps = 4096;
// Steps = 2 ^ #ofADCpins
// Setup and Loop

void setup(void)
{
  startSerial2();

  startCustomWatchDog();
  
  // disable unused components and hardware pins //
  componentsAlwaysOff();
  //hardwarePinsAlwaysOff(); // TODO are we turning off I2C pins still, which is wrong

  setupSwitchedPower();

  Serial2.println("hello");
  enableSwitchedPower();

  setupHardwarePins();
  Serial2.println("hello");
  Serial2.flush();

  //Serial2.println(atlasRGBSensor.get_name());
  // digitalWrite(PA4, LOW); // turn on the battery measurement

  //blinkTest();
  

  // Set up the internal RTC
  RCC_BASE->APB1ENR |= RCC_APB1ENR_PWREN;
  RCC_BASE->APB1ENR |= RCC_APB1ENR_BKPEN;
  PWR_BASE->CR |= PWR_CR_DBP; // Disable backup domain write protection, so we can write
  

  // delay(20000);

  allocateMeasurementValuesMemory();

  setupManualWakeInterrupts();

  powerUpSwitchableComponents();
  delay(2000);

   // Don't respond to interrupts during setup
  disableManualWakeInterrupt();
  clearManualWakeInterrupt();

  AtlasRGB::instance()->start(&Wire2);
  
  // Setup RGB Sensor
  AtlasRGB::instance()->deviceInformation();
  AtlasRGB::instance()->sendCommand();
  Serial2.println(AtlasRGB::instance()->receiveResponse());

  // Clear the alarms so they don't go off during setup
  clearAllAlarms();

  initializeFilesystem();

  //initBLE();

  readUniqueId(uuid);

  setNotBursting(); // prevents bursting during first loop

  //awakeTime = timestamp(); // Push awake time forward and provide time for user interation during setup()

  /* We're ready to go! */
  Monitor::instance()->writeDebugMessage(F("done with setup"));
  Serial2.flush();

  print_debug_status(); 
}

extern "C" char* _sbrk(int incr);
int freeMemory(){
  char top;
  Serial2.println( (int) &top );
  Serial2.println( (int) reinterpret_cast<char*>(_sbrk(0)) );

  return &top - reinterpret_cast<char*>(_sbrk(0));
}

void intentionalMemoryLeak(){
  // cause a memory leak
  char * mem = (char *) malloc(400); // intentional memory leak, big enough to get around buffering
  Serial2.println(mem); // use it so compiler doesn't remove the leak
}

/* main run loop order of operation: */
void loop(void)
{
  CH4s = analogRead(CH4sens); //read CH4 Vout
  CH4smV = CH4s*(mV/steps); //convert pin reading to mV
  if (CH4smV < minS) {
    minS = CH4smV;
  } 
  delay(10); //delay between reading of different analogue pins adviced.
  CH4r = analogRead(CH4ref); //read CH4 Vref
  CH4rmV = CH4r*(mV/steps); //convert pin reading to mV
  if (CH4rmV < minR) {
    minR = CH4rmV;
  } 
  delay(10); //delay between reading of different analogue pins adviced.
  Vbat = analogRead(Vb); //read CH4 Vref
  VbatmV = Vbat *(mV/steps); //convert pin reading to mV, NOT YET correcting for the voltage divider.
  if (VbatmV < minV && VbatmV != 0) {
    minV = VbatmV;
  } 
  delay(10); //delay between reading of different analogue pins adviced.
  Serial2.println("Next Set:");
  Serial2.println(CH4s);
  Serial2.println(CH4smV);
  Serial2.println(minS);
  Serial2.println();
  Serial2.println(CH4r);
  Serial2.println(CH4rmV);
  Serial2.println(minR);
  Serial2.println();
  Serial2.println(Vbat);
  Serial2.println(VbatmV);
  Serial2.println(minV);
  Serial2.println();
  
  startCustomWatchDog();
  printWatchDogStatus();

  // Get reading from RGB Sensor
  AtlasRGB::instance()->singleMode();
  AtlasRGB::instance()->sendCommand();
  Serial2.println(AtlasRGB::instance()->receiveResponse());

  // calculate and print free memory
  // reset the system if we are running out of memory
  char freeMemoryMessage[21];
  int freeMemoryAmount = freeMemory();
  sprintf(freeMemoryMessage, "Free Memory: %d", freeMemoryAmount);
  Monitor::instance()->Monitor::instance()->writeSerialMessage(freeMemoryMessage);
  if(freeMemoryAmount < 500){
    Monitor::instance()->Monitor::instance()->writeSerialMessage(F("Low memory, resetting!"));
    nvic_sys_reset(); // software reset, takes us back to init
  }

  // allocate and free the ram to test if there is enough?
  //nvic_sys_reset - what does this do?

  bool bursting = checkBursting();
  bool debugLoop = checkDebugLoop();
  bool awakeForUserInteraction = checkAwakeForUserInteraction(debugLoop);
  bool takeMeasurement = checkTakeMeasurement(bursting, awakeForUserInteraction);

  // Should we sleep until a measurement is triggered?
  bool awaitMeasurementTrigger = false;
  if (!bursting && !awakeForUserInteraction)
  {
    Monitor::instance()->writeDebugMessage(F("Not bursting or awake"));
    awaitMeasurementTrigger = true;
  }

  if (awaitMeasurementTrigger) // Go to sleep
  {
    stopAndAwaitTrigger();
    return; // Go to top of loop
  }

  if (WaterBear_Control::ready(Serial2))
  {
    handleControlCommand();
    return;
  }

  if (WaterBear_Control::ready(getBLE()))
  {
    Monitor::instance()->writeDebugMessage(F("BLE Input Ready"));
    awakeTime = timestamp(); // Push awake time forward
    WaterBear_Control::processControlCommands(getBLE());
    return;
  }

  if (takeMeasurement)
  {
    takeNewMeasurement();
    trackBurst(bursting);
    if (DEBUG_MEASUREMENTS)
    {
      monitorValues();
    }
  }

  if (configurationMode)
  {
    monitorConfiguration();
  }

  if (debugValuesMode)
  {
    if (burstCount == burstLength) // will cause loop() to continue until mode turned off
    {
      prepareForTriggeredMeasurement();
    }
    monitorValues();
  }

  if (tempCalMode)
  {
    monitorTemperature();
  }
}
