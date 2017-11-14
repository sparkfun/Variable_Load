#include "project.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "terminal.h"

#define KP  0.15
#define KI  0.1
#define KD  0.0

#define DEFAULT_I_LIM 0.0

// systemTimer is incremented in the tickISR, which occurs once a millisecond.
//  It's the timebase for the entire firmware, upon which all other timing
//  is based.
volatile int32 systemTimer = 0;
CY_ISR_PROTO(tickISR);

int main(void)
{
  CyGlobalIntEnable; /* Enable global interrupts. */
  
  // ARM devices have an internal system tick which can be used as a timebase
  // without having to tie up additional system resources. Here we set it up to
  // point to our system tick ISR and configure it to occur once every 64000
  // clock ticks, which is once per millisecond on our 64MHz processor.
  CyIntSetSysVector((SysTick_IRQn + 16), tickISR);
  SysTick_Config(64000);
  
  // Create our timing "tick" variables. These are used to record when the last
  // iteration of an action in the main loop happened and to trigger the next
  // iteration when some number of clock ticks have passed.
  int32_t _10HzTick = 0;
  int32_t _1kHzTick = 0;
  
  Gate_Drive_Tune_Start();
  Offset_Start();
  Offset_Gain_Start();
  GDT_Buffer_Start();
  O_Buffer_Start();
  Source_ADC_Start();
  I_Source_ADC_Start();
  
  float iLimit = DEFAULT_I_LIM;
  float error = 0.0;
  float lastError = 0.0;
  float integral = 0.0;
  float derivative = 0.0;
  float vSource = 0.0;
  int16_t vSourceRaw = 0;
  float iSource = 0.0;
  float setPoint = 0.0;
  int32_t iSourceRaw = 0;
  uint16_t grossSetPoint = 64;
  uint16_t fineSetPoint = 0;
  
  bool upPressed = false;
  bool downPressed = false;
  bool entPressed = false;
  bool backPressed = false;
  bool enableOutput = true;
  
  char buff[64];
  uint8_t inBuff[64];
  uint8_t floatBuff[64];
  uint8_t incCharIndex = 0;
  
  //USBUART_Start(0, USBUART_5V_OPERATION);
  CapSense_Start();
  CapSense_InitializeAllBaselines();
  UART_Start();
  //LCD_Start();
  
  //init();
  for(;;)
  {
   /* if (0u != USBUART_IsConfigurationChanged())
    {
      // Initialize IN endpoints when device is configured.
      if (0u != USBUART_GetConfiguration())
      {
        // Enumeration is done, enable OUT endpoint to receive data 
        // from host. 
        USBUART_CDC_Init();
      }
    }*/
    if(0u == CapSense_IsBusy())
    {
      /* Update all baselines */
       CapSense_UpdateEnabledBaselines();

      /* Start scanning all enabled sensors */
       CapSense_ScanEnabledWidgets();
    }
    
    // Handle a press of the back key
    if (CapSense_CheckIsWidgetActive(CapSense_BACK__BTN) && backPressed == false)
    {
      UART_PutString("Button BACK pressed\n\r");
      backPressed = true;
      iLimit = DEFAULT_I_LIM;
    }
    else if (!CapSense_CheckIsWidgetActive(CapSense_BACK__BTN) && backPressed == true)
    {
      backPressed = false;
    }
    
    if (CapSense_CheckIsWidgetActive(CapSense_ENTER__BTN) && entPressed == false)
    {
      UART_PutString("Button ENTER pressed\n\r");
      entPressed = true;
      enableOutput = !enableOutput;
      if (enableOutput)
      {
        Output_On_LED_Write(1);
      }
      else
      {
        Output_On_LED_Write(0);
      }
    }
    else if (!CapSense_CheckIsWidgetActive(CapSense_ENTER__BTN) && entPressed == true)
    {
      entPressed = false;
    }
    
    if (CapSense_CheckIsWidgetActive(CapSense_DOWN__BTN) && downPressed == false)
    {
      UART_PutString("Button DOWN pressed\n\r");
      downPressed = true;
      if (iLimit < .01) iLimit = 0;
      else if (iLimit < 0.101) {if (iLimit >= 0.01) iLimit -= 0.01;}
      else if (iLimit < 0.501) iLimit -= 0.05;
      else if (iLimit < 1.001) iLimit -= 0.1;
      else iLimit -= 0.5;
    }
    else if (!CapSense_CheckIsWidgetActive(CapSense_DOWN__BTN) && downPressed == true)
    {
      downPressed = false;
    }
    
    if (CapSense_CheckIsWidgetActive(CapSense_UP__BTN) && upPressed == false)
    {
      UART_PutString("Button UP pressed\n\r");
      upPressed = true;
      if (iLimit < 0.099) iLimit += 0.01;
      else if (iLimit < 0.5) iLimit += 0.05;
      else if (iLimit < 1.0) iLimit += 0.1;
      else if (iLimit < 3.5) iLimit += 0.5;
      else iLimit = 4.0;
    }
    else if (!CapSense_CheckIsWidgetActive(CapSense_UP__BTN) && upPressed == true)
    {
      upPressed = false;
    }
/*    
    if (systemTimer - 1 > _1kHzTick)
    {
      _1kHzTick = systemTimer;
      
      I_Source_ADC_StartConvert();
      I_Source_ADC_IsEndConversion(I_Source_ADC_WAIT_FOR_RESULT);
      iSourceRaw = I_Source_ADC_GetResult32();
      iSource = 10 * I_Source_ADC_CountsTo_Volts(iSourceRaw);
      
      lastError = error;
      error = iLimit - iSource;
      integral = integral + error;
      derivative = error - lastError;
      setPoint = (KP * error) + (KI * integral) + (KD * derivative);

      // setPoint is a floating point voltage. We need to convert that
      //  into an integer that can be fed into our DACs.
      // First, find our grossSetPoint. This is a largish voltage that
      //  represents a coarse 0-4V offset in 16mV steps.
      grossSetPoint = (int)(setPoint/0.016);
      // We want to limit our gross offset to 255, since it's an 8-bit
      //  DAC output.
      if (grossSetPoint > 255) grossSetPoint = 255;
      // Now, find the fineSetPoint. This is a 4mV step 8-bit DAC which
      //  allows us to tune the set point a little more finely.
      fineSetPoint = (int)((setPoint - (float)grossSetPoint*0.016)/0.004);
      // Finally, one last check: if the source voltage is below 2.0V,
      //  the output is disabled, or the total power is greater than 15W,
      //  we want to clear everything and zero the gate drive.
      Source_ADC_StartConvert();
      Source_ADC_IsEndConversion(Source_ADC_WAIT_FOR_RESULT);
      vSourceRaw = Source_ADC_GetResult16();
      vSource = 10*Source_ADC_CountsTo_Volts(vSourceRaw);
      if ((vSource < 2.0) ||
          (enableOutput == false) ||
          (vSource * iLimit > 15))
      {
        error = 0;
        lastError = 0;
        integral = 0;
        derivative = 0;
        grossSetPoint = 0;
        fineSetPoint = 0;
      }
      Offset_SetValue(grossSetPoint);
      Gate_Drive_Tune_SetValue(fineSetPoint);
      
      // Fetch any waiting characters from the USB UART.
      int charCount = USBUART_GetCount();
      if (charCount > 0)
      {
        int i = USBUART_GetAll(inBuff);
        
        for (i = 0; i < charCount; i++)
        {
          floatBuff[incCharIndex++] = inBuff[i];
          if (incCharIndex > 63) incCharIndex = 0;
          if (inBuff[i] == '\r' ||
              inBuff[i] == '\n')
          {
            floatBuff[incCharIndex] = '\0';
            break;
          }
        }
      }
    } // if (systemTimer - 50 > _20HzTick)
        
    if (systemTimer - 100 > _10HzTick)
    {
      _10HzTick = systemTimer;
      
      //while (0u == USBUART_CDCIsReady());
      cls();
      goToPos(1,1);
      putString("I Source:");
      goToPos(1,2);
      putString("I Limit:");
      goToPos(1,3);
      putString("V Source:");
      goToPos(12,1);
      if (iSource < 0) iSource *= -1;
      sprintf(buff, "%.3f", iSource);
      putString(buff);
      goToPos(12,2);
      sprintf(buff, "%.3f", iLimit);
      putString(buff);
      goToPos(12,3);
      sprintf(buff, "%.3f", vSource);
      putString(buff);
      //sprintf(buff, "Set point: %.3f", setPoint);
      //USBUART_PutString(buff);

      if (floatBuff[incCharIndex] == '\0')
      {
        iLimit = atoff((char*)floatBuff);
        incCharIndex = 0;
        if (iLimit > 4.0 ||
            iLimit < 0.0) iLimit = 0.0;
      }
    }
*/
  }
}

CY_ISR(tickISR)
{
  systemTimer++;
}
