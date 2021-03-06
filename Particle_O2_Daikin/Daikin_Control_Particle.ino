/*
	O2Land RHT Dakin Control
	RHT-Daikin-Ctrl Arduino Module is needed
	
	Remote Trigger Command
	message to o2daikin
	command:
		daikin-auto                 enable auto control
		daikin-off                  turn OFF (but auto control is still there)
		rht-disable                 disable auto control

	Author: Samson Chen, Qblinks Inc.
	Date: May 15th, 2016
*/

#include "elapsedMillis.h"

// ambient environmental parameters
// Note: the default cooling temperature is set to 24.00 in RHT_Daikin_Ctrl.ino
//
// if off, T > STD = ac, else R > STD = de
// if ac, T <= STD, consider mode change
//                 R > STD = de
// if de, T > HIGH, mode change

#define TEMP_STD_MODE1          25.00
#define TEMP_STD_MODE2          26.00
#define TEMP_HIGH               27.00
#define RH_STD                  60.50

#define TEMP_TOO_LOW_MODE1      23.95
#define TEMP_TOO_LOW_MODE2      24.95

#define HEAT_INDEX_TOO_HIGH     27.50       // use Heat Index to check if the ambient condition needs to be changed, if so, KEEP_ONOFF_TIMER will be ignored

// running time in the first hour with the high AC-FAN speed/FAN on
#define	FIRST_HOUR_HIGH_FAN_MIN 35

// time that allows OFF
//   if the current time is outside this time window, once the AC is on, it will switch between COOLING and DEHUMIDIFIER
//   during this time window, it is allowed to turn AC off
#define OFFOK_BEGIN_HOUR        4
#define OFFOK_BEGIN_MIN         00          // 4:00 AM
#define OFFOK_END_HOUR          5
#define OFFOK_END_MIN           30          // 5:30 AM

// avoid frequent ON-OFF mode switch
//   when the mode is switched, no more action is allowed before this timer is reached
#define KEEP_ONOFF_TIMER        25          // in munutes

// must off time
//   this auto system cannot exceed MUST_OFF_TIMER
//   once this timer is reached, the AC must be turned off 
//   another daikin-auto command is needed to restart the system
#define MUSTOFFTIMER_REGULAR    720         // in minutes
#define	MUSTOFFTIMER_SHORT		120			// if daikin-auto command is received during certain time of period, use a shorter timer
#define	MUSTOFFTIMER_SHORT_BEGIN 17	  		// 5:00:00 PM
#define	MUSTOFFTIMER_SHORT_END	20	  		// until 8:59:59 PM

// status and control parameters
#define MODE_OFF                0
#define MODE_COOLING            1
#define MODE_DEHUMIDIFIER       2

#define IR_REPEAT               3
#define IR_REPEAT_DELAY         2000

#define TURN_FAN_ON             TRUE
#define DONT_CTRL_FAN           FALSE

bool rhtDisabled = true;        // overall system switch

unsigned int must_off_timer = MUSTOFFTIMER_REGULAR;

int currentMode = MODE_OFF;
elapsedMillis elapsed_must_off_timer;
elapsedMillis elapsed_keep_onoff_timer;
elapsedMillis elapsed_exterem_cond_timer;

String recvLine = "";
elapsedMillis timeElapsed;
elapsedMillis timeElapsedSyncTime;
elapsedMillis timeElapsedResetSht;

float currentTemp = 0;
float currentRh = 0;
float currentHI = 0;
unsigned int currentReadCount = 0;

bool current_temp_mode1 = true;
bool current_fan_mode_on = false;
bool ac_already_off = false;    // even during AC-OFF-OK period, it can be only one time AC off
bool first_hour_mode = false;   // first hour use stronger AC-FAN speed and turn on the FAN

// -----------------------------------------------------------------------------------
void setup() {
		Serial1.begin(38400);

		// Blue LED
		pinMode(D7, OUTPUT);
		
		// Turn RGB LED off
		RGB.control(true);
		RGB.color(0, 0, 0);
		delay(1000);
		RGB.brightness(0);
		delay(1000);

		// Subscribe Network Event
		Particle.subscribe("o2daikin", myHandler);
		
		// sync timer and set time zone
		Particle.syncTime();
		Time.zone(+8);
		
		// reset elapsed timer
		timeElapsed = 0;
		timeElapsedSyncTime = 0;
		timeElapsedResetSht = 0;
		
		// maximize must-off timer to ensure the initialized state is not triggering anything
		elapsed_must_off_timer = MUSTOFFTIMER_REGULAR * (long) 60000;
		
		// zero the no-onoff timer to avoid any sudden action
		elapsed_keep_onoff_timer = 0;
		
		// reset the extreme interval timer
		elapsed_exterem_cond_timer = 0;
		
		// system is by default off until the first enable command is sent in
		rhtDisabled = true;

		// other default state settings
		must_off_timer = MUSTOFFTIMER_REGULAR;
		current_temp_mode1 = true;
		current_fan_mode_on = false;
		ac_already_off = false;
		first_hour_mode = false;

		// set mode 1 temperature to the air conditioning      
		Serial1.println("att24");
						
		// log the event
		Particle.publish("o2sensor", "RhT Control System Initialized, 2016-08-27");
}


void fan_on()
{
		if(!current_fan_mode_on)
		{
				// control through IFTTT
				Particle.publish("o2fan", "ON");
				
				// set the flag
				current_fan_mode_on = TRUE;
				
				// log the event
				Particle.publish("o2sensor", "Turn fan ON");
		}
}


void fan_off()
{
		if(current_fan_mode_on)
		{
				// control through IFTTT
				Particle.publish("o2fan", "OFF");
				
				// set the flag
				current_fan_mode_on = FALSE;
				
				// log the event
				Particle.publish("o2sensor", "Turn fan OFF");
		}
}


void daikin_ac_on()
{
		unsigned int repeat;
		
		// convert the overall running time of auto control to minutes
		unsigned int ignoranceTimerInMinutes = elapsed_must_off_timer / (long) 60000;
		
		// send to log
		Particle.publish("o2sensor", "switch to cooling");
		
		// repeatly sending commands
		for(repeat = 0; repeat < IR_REPEAT; repeat++)
		{
				// send ON ac command
				Serial1.println("atc1");
				
				// delay per repeated command
				delay(IR_REPEAT_DELAY);
		}
		
		// set mode
		currentMode = MODE_COOLING;
		
		// reset the on-off control timer
		elapsed_keep_onoff_timer = 0;
		
		// fan off if this is not the first hour
		if(ignoranceTimerInMinutes >= FIRST_HOUR_HIGH_FAN_MIN)
		{
				fan_off();
		}
}


void daikin_dehumidifier_on()
{
		unsigned int repeat;
		
		// convert the overall running time of auto control to minutes
		unsigned int ignoranceTimerInMinutes = elapsed_must_off_timer / (long) 60000;
		
		// send to log
		Particle.publish("o2sensor", "switch to dehumidifier");
		
		// repeatly sending commands
		for(repeat = 0; repeat < IR_REPEAT; repeat++)
		{
				// send ON dehumidifier command
				Serial1.println("atm1");
				
				// delay per repeated command
				delay(IR_REPEAT_DELAY);
		}
		
		// set mode
		currentMode = MODE_DEHUMIDIFIER;
		
		// reset the on-off control timer
		elapsed_keep_onoff_timer = 0;
		
		// fan off if this is not the first hour
		if(ignoranceTimerInMinutes >= FIRST_HOUR_HIGH_FAN_MIN)
		{
				fan_off();
		}
}


void daikin_off(bool turnFanOn)
{
		unsigned int repeat;
		
		// send to log
		Particle.publish("o2sensor", "turn off");
		
		// repeatly sending commands
		for(repeat = 0; repeat < IR_REPEAT; repeat++)
		{
				// send OFF command
				Serial1.println("atd0");
				
				// delay per repeated command
				delay(IR_REPEAT_DELAY);
		}
		
		// set mode
		currentMode = MODE_OFF;
		
		// reset the on-off control timer
		elapsed_keep_onoff_timer = 0;
		
		// FAN control
		if(turnFanOn)
		{
				fan_on();
		}
		else
		{
				fan_off();
		}
}

// =========================================================================================================
// =========================================================================================================
// =========================================================================================================

void loop() 
{
		// ---------------------------------------------------------------------------------------------------------------------------------
		// Convert stat timers
		// ---------------------------------------------------------------------------------------------------------------------------------
		
		// convert the time of the last command sent to minutes
		unsigned int lastCommandSentInMinutes = elapsed_keep_onoff_timer / (long) 60000;

		// convert the overall running time of auto control to minutes
		unsigned int ignoranceTimerInMinutes = elapsed_must_off_timer / (long) 60000;
		
		// ---------------------------------------------------------------------------------------------------------------------------------
		// if this is the first hour of rht-auto service on, set stronger AC-FAN speed and turn on the FAN
		// otherwise, set it back
		// ---------------------------------------------------------------------------------------------------------------------------------
		if(ignoranceTimerInMinutes < FIRST_HOUR_HIGH_FAN_MIN)
		{
				if(!first_hour_mode)
				{
						// log the event
						Particle.publish("o2sensor", "set the first hour mode");
						
						// set the flag
						first_hour_mode = true;
						
						// set the stronger AC-FAN speed
						Serial1.println("atf4");

						// and lower temperature      
						Serial1.println("att21");
						
						// turn on FAN 
						fan_on();            
				}
		}
		else
		{
				if(first_hour_mode)
				{
						// log the event
						Particle.publish("o2sensor", "unset the first hour mode");
						
						// set the flag back
						first_hour_mode = false;
						
						// set normal quiet AC-FAN speed
						Serial1.println("atf6");
						
						// and normal temperature      
						Serial1.println("att24");
						
						// turn off FAN
						fan_off();
						
						// reset the AC or Dehumidifier command to set the AC-FAN speed back
						switch(currentMode)
						{
								case MODE_COOLING:
										daikin_ac_on();
										break;
										
								case MODE_DEHUMIDIFIER:
										daikin_dehumidifier_on();
										break;
										
								default:
										break;
						}            
				}
		}

		// ---------------------------------------------------------------------------------------------------------------------------------
		// Standard Temperature is different in different time
		// ---------------------------------------------------------------------------------------------------------------------------------
		float standard_temp = TEMP_STD_MODE1;
		float too_low_temp = TEMP_TOO_LOW_MODE1;

		// ---------------------------------------------------------------------------------------------------------------------------------
		// Processing periodical commands
		// ---------------------------------------------------------------------------------------------------------------------------------
		
		if (timeElapsed > (long) 60000) 
		{
				Serial1.println("atrt");        // send command of getting environmental information
				timeElapsed = 0;                // reset the counter to 0 so the counting starts over...
				
				// debugging area
				//Particle.publish("o2sensor", String(Time.hour()) + ":" + String(Time.minute()));
		}
		
		// daily time sync
		if(timeElapsedSyncTime > 43200000)
		{
				Particle.syncTime();
				timeElapsedSyncTime = 0;
		}
		
		// reset SHT31-D every hour plus 7 seconds
		// it seems SHT31-D some time freeze after constantly running for a few days
		if(timeElapsedResetSht > (long) 3607000)
		{
				Serial1.println("atrs");        // send command to reset SHT31-D
				timeElapsedResetSht = 0;
		}
	
		// ---------------------------------------------------------------------------------------------------------------------------------
		// Check OFFOK window
		// ---------------------------------------------------------------------------------------------------------------------------------
		bool offOK;
		unsigned int minutesOfToday = Time.hour() * 60 + Time.minute();
		unsigned int minutesOfWindowBegin = OFFOK_BEGIN_HOUR * 60 + OFFOK_BEGIN_MIN;
		unsigned int minutesOfWindowEnd = OFFOK_END_HOUR * 60 + OFFOK_END_MIN;
		
		if(minutesOfToday >= minutesOfWindowBegin && minutesOfToday <= minutesOfWindowEnd)
		{
				offOK = true;
				
				// use temperature mode 2 during this time period
				standard_temp = TEMP_STD_MODE2;
				too_low_temp = TEMP_TOO_LOW_MODE2;
				
				// switch temperature mode if this is the new state
				if(current_temp_mode1)
				{
						current_temp_mode1 = false;
						
						// set mode 2 temperature to the air conditioning      
						Serial1.println("att25");
						
						// log the event
						Particle.publish("o2sensor", "switch to temperature mode 2");

						// send the IR command again with the new temperature setting
						if(!rhtDisabled)
						{
							switch(currentMode)
							{
									case MODE_COOLING:
											daikin_ac_on();
											break;
											
									case MODE_DEHUMIDIFIER:
											daikin_dehumidifier_on();
											break;
											
									default:
											break;
							}            
						}            
				}
		}
		else
		{
				offOK = false;
				
				// use temperature mode 1 during this time period
				standard_temp = TEMP_STD_MODE1;
				too_low_temp = TEMP_TOO_LOW_MODE1;
				
				// switch temperature mode if this is the new state
				if(!current_temp_mode1)
				{
						current_temp_mode1 = true;
						
						// set mode 1 temperature to the air conditioning      
						Serial1.println("att24");
						
						// log the event
						Particle.publish("o2sensor", "switch to temperature mode 1");

						// send the IR command again with the new temperature setting
						if(!rhtDisabled)
						{
							switch(currentMode)
							{
									case MODE_COOLING:
											daikin_ac_on();
											break;
											
									case MODE_DEHUMIDIFIER:
											daikin_dehumidifier_on();
											break;
											
									default:
											break;
							}            
						}            
				}
		}

		// ---------------------------------------------------------------------------------------------------------------------------------
		// Processing incoming UART commands
		// ---------------------------------------------------------------------------------------------------------------------------------
		
		// check if Qmote has incoming notifications or commands
		while (Serial1.available() > 0)
		{
				char c = Serial1.read();
				
				if(c == 0x0D)
				{
						// envrionmental data response
						if(recvLine.substring(0, 3)  == ">R=")
						{
								// proceed extraction
								
								int searchIdx;
								int commaIdx;
								float xRh = 0;
								float xTemp = 0;
								float xHI = 0;
								unsigned int xReadCount = 0;
								bool xtracted = false;

								// extract R
								searchIdx = recvLine.indexOf("R=");
								if(searchIdx >= 0)
								{
										commaIdx = recvLine.substring(searchIdx).indexOf(",");
										if(commaIdx >= 0)
										{
												xRh=recvLine.substring(searchIdx + 2, commaIdx + searchIdx).toFloat();
												
												// extract T
												searchIdx = recvLine.indexOf("T=");
												if(searchIdx >= 0)
												{
														commaIdx = recvLine.substring(searchIdx).indexOf(",");
														if(commaIdx >= 0)
														{
																xTemp=recvLine.substring(searchIdx + 2, commaIdx + searchIdx).toFloat();
																
																// extract H
																searchIdx = recvLine.indexOf("H=");
																if(searchIdx >= 0)
																{
																		commaIdx = recvLine.substring(searchIdx).indexOf(",");
																		if(commaIdx >= 0)
																		{
																				xHI = recvLine.substring(searchIdx + 2, commaIdx + searchIdx).toFloat();
																
																				// extract C
																				searchIdx = recvLine.indexOf("C=");
																				if(searchIdx >= 0)
																				{
																						xReadCount = recvLine.substring(searchIdx + 2).toInt();
																						
																						// information successfully extracted
																						xtracted = true;
																				}
																		}
																}
														}
												}
										}
								}

								// apply the information if extracted
								if(xtracted)
								{
										currentRh = xRh;
										currentTemp = xTemp;
										currentHI = xHI;
										currentReadCount = xReadCount & 0xFFFF;     // keep only the low 16-bit
										String modeStr;
										
										// output current mode
										switch(currentMode)
										{
												case MODE_OFF:
														modeStr = "OFF";
														break;
														
												case MODE_COOLING:
														modeStr = "AC";
														break;
														
												case MODE_DEHUMIDIFIER:
														modeStr = "DH";
														break;
														
												default:
														modeStr = "Unknown";
														break;
										}
										
										// publish to thingspeak
										Particle.publish("thingspeak", "field1=" + String(xRh, 2) + "&field2=" + String(xTemp, 2) + "&field3=" + String(xHI,2));
										
										// publish to the event log
										String txtOutput = "Rh" + String(xRh, 2) + ", T" + String(xTemp, 2) + ", HI" + String(xHI,2) + ", C" + String(currentReadCount) + ", " + modeStr;
										
										// added the conditional stat events
										if(rhtDisabled)
										{
												txtOutput += ", RhT-Disabled";
										}
										else if(ignoranceTimerInMinutes >= must_off_timer)
										{
												txtOutput += ", MUST_OFF_STAT";
										}
										else if(lastCommandSentInMinutes < KEEP_ONOFF_TIMER)
										{
												txtOutput += ", NO-ONOFF";
										}
										else if(offOK == true)
										{
												txtOutput += ", OFF-OK";
										}
										
										// publish it
										Particle.publish("o2sensor", txtOutput);
								} // end if(xtracted)
						} // end of extraction
						
					// clear the line
					recvLine.remove(0);
					recvLine = "";
				}
				else if(c != 0x0A)
				{
					recvLine += c;
				}
				// ignore 0x0A
		} // end while

		// ---------------------------------------------------------------------------------------------------------------------------------
		// Environmental Control
		// ---------------------------------------------------------------------------------------------------------------------------------

		if(!rhtDisabled)
		{
				// auto control must be running during the allowed time period
				if(ignoranceTimerInMinutes < must_off_timer)
				{
						// extreme condition that could bypass the on-off timer (temperature change too fast)
						if(
							 ((currentMode == MODE_OFF && ((float) currentHI) >= ((float) HEAT_INDEX_TOO_HIGH)) && currentMode != MODE_COOLING)   // heat index too high AND not in AC
							 ||                                                                                                                   // OR
							 ((currentMode != MODE_OFF && ((float) currentTemp) <= ((float) too_low_temp)) && currentMode == MODE_DEHUMIDIFIER)   // temperature too low AND still in DE
							)
						{
								// extreme condition check can be triggered once a minute only
								// otherwise, before the ambient environment is changed, the same factor repeately triggers the same mode change request
								if(elapsed_exterem_cond_timer >= (long) 60000)
								{
										// maximize the on-off timer to allow the next contorl
										elapsed_keep_onoff_timer = KEEP_ONOFF_TIMER * (long) 60000;
										
										// reset the extreme condition timer
										elapsed_exterem_cond_timer = 0;
								}
						}
		
						// avoid frequent mode switching
						if(lastCommandSentInMinutes >= KEEP_ONOFF_TIMER)
						{
								// .....................................................
								
								if(currentMode == MODE_OFF)
								{
										if((float) currentTemp > (float) standard_temp)
										{
												// ac-priority
												daikin_ac_on();
										}
										else
										{
												// Rh-priority
												if((float) currentRh > (float) RH_STD)
												{
														daikin_dehumidifier_on();
												}
										}
								}
								
								// .....................................................
								
								else if(currentMode == MODE_COOLING)
								{
										if((float) currentTemp <= (float) standard_temp)            // consider mode change
										{
												if((float) currentRh > (float) RH_STD)
												{
														daikin_dehumidifier_on();
												}
												else if((float) currentTemp <= (float) too_low_temp)    // this should not happen since the temp is controlled
												{
														if(offOK && (!ac_already_off))
														{
																daikin_off(TURN_FAN_ON);                    // if this really happens and it is OK to off, then off and turn fan ON
																
																// ac can be off only once during the OFF-OK period, 
																// otherwise, it indicates the ambient environment is extereme
																ac_already_off = true;

																// log the event
																Particle.publish("o2sensor", "AC is allowed to be off once");
														}
														else
														{
																daikin_ac_on();                                 // this happens only when the mode is changed manully outside this auto control
														}
												}
										}
								}
								
								// .....................................................
								
								else    // MODE_DEHUMIDIFIER
								{
										if((float) currentTemp <= (float) too_low_temp)             // temp  too low, happens when it is very humid
										{
												if(offOK && (!ac_already_off))
												{
														daikin_off(TURN_FAN_ON);                        // if OK to off, then off and turn fan ON
																
														// ac can be off only once during the OFF-OK period, 
														// otherwise, it indicates the ambient environment is extereme
														ac_already_off = true;
														
														// log the event
														Particle.publish("o2sensor", "AC is allowed to be off once");
												}
												else
												{
														daikin_ac_on();                                     // switch to cooling mode so the temperature is re-controlled
												}
										}
										else if((float) currentTemp > (float) TEMP_HIGH)            // mode change
										{
												daikin_ac_on();
										}
								}
								
								// .....................................................
						} // end on-off control
				} // end must-off control
		
				// ------------------------------------------------------------------------------------------
		
				// stop the system if the MUST_OFF_TIMER is reached
				if(ignoranceTimerInMinutes >= must_off_timer)
				{
						if(currentMode != MODE_OFF)
						{
								// send off command now
								daikin_off(DONT_CTRL_FAN);
						}
						else if(lastCommandSentInMinutes >= KEEP_ONOFF_TIMER)
						{
								// repeatedly sending off command for every KEEP_ONOFF_TIMER 
								daikin_off(DONT_CTRL_FAN);
						}
				}
				
				// ------------------------------------------------------------------------------------------
				
		} // end rhtDisabled
} // end main loop


// Event handler triggered from IFTTT
void myHandler(const char *event, const char *data)
{
		String ingredient = data;

		if(ingredient.indexOf("daikin-auto") > -1)
		{   
				// log the event
				Particle.publish("o2sensor", "RhT-Auto Enabled");
				
				// determine the MUST_OFF_TIMER based on current time
				if(Time.hour() >= MUSTOFFTIMER_SHORT_BEGIN && Time.hour() <= MUSTOFFTIMER_SHORT_END)
				{
						must_off_timer = MUSTOFFTIMER_SHORT;
						
						// log the event
						Particle.publish("o2sensor", "use short MUST_OFF_TIMER");
				}
				else
				{
						must_off_timer = MUSTOFFTIMER_REGULAR;
						
						// log the event
						Particle.publish("o2sensor", "use regular MUST_OFF_TIMER");
				}

				// maximize the on-off timer to allow the next contorl
				elapsed_keep_onoff_timer = KEEP_ONOFF_TIMER * (long) 60000;
				
				// reenable the RHT control system
				rhtDisabled = false;
				ac_already_off = false;
				
				// check if this is a fresh start or reissue to skip the first hour mode
				if(first_hour_mode)
				{
					// this is a reissued command to skip the first hour mode
					// move the time ahead, assume the first hour mode has been run
					elapsed_must_off_timer = FIRST_HOUR_HIGH_FAN_MIN * (long) 60000;
						
					// log the event
					Particle.publish("o2sensor", "manually skip the first hour");
				}
				else
				{
					// this is a fresh start        	
					// reset must-off timer to enable the auto control
					elapsed_must_off_timer = 0;
				}
				
				// assume the current mode off
				currentMode = MODE_OFF;
		}
		else if(ingredient.indexOf("daikin-off") > -1)
		{
				daikin_off(DONT_CTRL_FAN);
				
				// maximize the must-off-timer to stop the auto-control
				elapsed_must_off_timer = MUSTOFFTIMER_REGULAR * (long) 60000;
				
				// reenable the RHT control system
				rhtDisabled = false;
		}
		else if(ingredient.indexOf("rht-disable") > -1)
		{
				// disable the RHT control system
				rhtDisabled = true;
				
				// log the event
				Particle.publish("o2sensor", "RhT System Disabled");
		}

		// toggle LED as a progress indicator
		digitalWrite(D7, HIGH);
		delay(1000);
		digitalWrite(D7, LOW);
}

