#include "project.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "terminal.h"

#define KPN 19
#define KPD 100
#define KIN 1
#define KID 10

#define DEFAULT_I_LIM 0
#define DEFAULT_V_MIN 2000

/* Defines for SourceDMA */
#define SourceDMA_BYTES_PER_BURST 2
#define SourceDMA_REQUEST_PER_BURST 1
#define SourceDMA_SRC_BASE (CYDEV_PERIPH_BASE)
#define SourceDMA_DST_BASE (CYDEV_SRAM_BASE)

/* Defines for BufferDMA */
#define BufferDMA_BYTES_PER_BURST 4
#define BufferDMA_REQUEST_PER_BURST 1
#define BufferDMA_SRC_BASE (CYDEV_PERIPH_BASE)
#define BufferDMA_DST_BASE (CYDEV_SRAM_BASE)

/* Variable declarations for BufferDMA */
/* Move these variable declarations to the top of the function */
uint8 BufferDMA_Chan;
uint8 BufferDMA_TD[1];

/* Defines for CurrentDMA */
#define CurrentDMA_BYTES_PER_BURST 4
#define CurrentDMA_REQUEST_PER_BURST 1
#define CurrentDMA_SRC_BASE (CYDEV_SRAM_BASE)
#define CurrentDMA_DST_BASE (CYDEV_SRAM_BASE)

/* Variable declarations for CurrentDMA */
/* Move these variable declarations to the top of the function */
static uint8 CurrentDMA_Chan;
static uint8 CurrentDMA_TD[1];

/* Variable declarations for SourceDMA */
/* Move these variable declarations to the top of the function */
#define ADCSAMPLES 40
static uint8 SourceDMA_Chan;
static uint8 SourceDMA_TD[1];
static volatile uint16 SourceData[ADCSAMPLES];

#define CURRENTSAMPLES 10
static volatile uint32 CurrentReadings[CURRENTSAMPLES];
static uint32 DMABuffer;

void DoPid();
void OutputEnable(bool v);

volatile static int32 systemTimer = 0;

static float fiLimit;
volatile static uint32 iLimit = DEFAULT_I_LIM;
volatile static uint16 vSource = 0;
volatile static int iSource = 0;
volatile static uint32 dt = 0;
volatile static int vMin = DEFAULT_V_MIN;
volatile static uint32 maHours;

bool enableOutput = false;

int main(void)
{
	static int i;
	static uint32 vAve;
	static char buff[64];
	static uint8 inBuff[64];
	static char floatBuff[64];
	static uint8_t incCharIndex = 0;

	CyGlobalIntEnable; /* Enable global interrupts. */

	// Create our timing "tick" variables. These are used to record when the last
	// iteration of an action in the main loop happened and to trigger the next
	// iteration when some number of clock ticks have passed.
	int32_t SlowTick = 0;

	Gate_Drive_Tune_Start();
	Offset_Start();
	Offset_Gain_Start();
	GDT_Buffer_Start();
	O_Buffer_Start();
	Source_ADC_Start();
	I_Source_ADC_Start();

	bool upPressed = false;
	bool downPressed = false;
	bool entPressed = false;
	bool backPressed = false;

	USBUART_Start(0, USBUART_5V_OPERATION);
	CapSense_Start();
	CapSense_InitializeAllBaselines();
	ConversionClock_Start();
	LCD_Start();
	LCD_DisplayOn();
	LCD_PrintString("Hello, world");

	/* DMA Configuration for SourceDMA */
	SourceDMA_Chan = SourceDMA_DmaInitialize(SourceDMA_BYTES_PER_BURST, SourceDMA_REQUEST_PER_BURST,
		HI16(SourceDMA_SRC_BASE), HI16(SourceDMA_DST_BASE));
	SourceDMA_TD[0] = CyDmaTdAllocate();
	CyDmaTdSetConfiguration(SourceDMA_TD[0], 2 * ADCSAMPLES, SourceDMA_TD[0], CY_DMA_TD_INC_DST_ADR);
	CyDmaTdSetAddress(SourceDMA_TD[0], LO16((uint32)Source_ADC_SAR_WRK0_PTR), LO16((uint32)SourceData));
	CyDmaChSetInitialTd(SourceDMA_Chan, SourceDMA_TD[0]);
	Source_ADC_StartConvert();
	CyDmaChEnable(SourceDMA_Chan, 1);

	/* DMA Configuration for BufferDMA */

	BufferDMA_Chan = BufferDMA_DmaInitialize(BufferDMA_BYTES_PER_BURST, BufferDMA_REQUEST_PER_BURST,
		HI16(BufferDMA_SRC_BASE), HI16(BufferDMA_DST_BASE));
	BufferDMA_TD[0] = CyDmaTdAllocate();
	CyDmaTdSetConfiguration(BufferDMA_TD[0], 4, BufferDMA_TD[0], TD_INC_SRC_ADR | BufferDMA__TD_TERMOUT_EN);
	CyDmaTdSetAddress(BufferDMA_TD[0], LO16((uint32)I_Source_ADC_DEC_SAMP_PTR), LO16((uint32)DMABuffer));
	CyDmaChSetInitialTd(BufferDMA_Chan, BufferDMA_TD[0]);

	/* DMA Configuration for CurrentDMA */
	CurrentDMA_Chan = CurrentDMA_DmaInitialize(CurrentDMA_BYTES_PER_BURST, CurrentDMA_REQUEST_PER_BURST,
		HI16(CurrentDMA_SRC_BASE), HI16(CurrentDMA_DST_BASE));
	CurrentDMA_TD[0] = CyDmaTdAllocate();
	CyDmaTdSetConfiguration(CurrentDMA_TD[0], 4 * CURRENTSAMPLES, CurrentDMA_TD[0], TD_INC_DST_ADR | CurrentDMA__TD_TERMOUT_EN);
	CyDmaTdSetAddress(CurrentDMA_TD[0], LO16((uint32)DMABuffer), LO16((uint32)CurrentReadings));
	CyDmaChSetInitialTd(CurrentDMA_Chan, CurrentDMA_TD[0]);

	/*Change the ADC coherent key to high byte*/
	I_Source_ADC_DEC_COHER_REG |= I_Source_ADC_DEC_SAMP_KEY_HIGH;
	I_Source_ADC_StartConvert();
	CyDmaChEnable(BufferDMA_Chan, 1);
	CyDmaChEnable(CurrentDMA_Chan, 1);

	PIDIsr_Start();

	init();

	for (;;)
	{
		if (0u != USBUART_IsConfigurationChanged())
		{
			if (0u != USBUART_GetConfiguration())
			{
				USBUART_CDC_Init();
			}
		}

		if (0u == CapSense_IsBusy())
		{
			CapSense_UpdateEnabledBaselines();
			CapSense_ScanEnabledWidgets();
		}

		// Handle a press of the back key
		if (CapSense_CheckIsWidgetActive(CapSense_BACK__BTN) && backPressed == false)
		{
			backPressed = true;
			iLimit = DEFAULT_I_LIM;
			vMin = DEFAULT_V_MIN;
			OutputEnable(false);
		}
		else if (!CapSense_CheckIsWidgetActive(CapSense_BACK__BTN) && backPressed == true)
		{
			backPressed = false;
		}

		if (CapSense_CheckIsWidgetActive(CapSense_ENTER__BTN) && entPressed == false)
		{
			entPressed = true;
			OutputEnable(!enableOutput);
		}
		else if (!CapSense_CheckIsWidgetActive(CapSense_ENTER__BTN) && entPressed == true)
		{
			entPressed = false;
		}

		if (CapSense_CheckIsWidgetActive(CapSense_DOWN__BTN) && downPressed == false)
		{
			downPressed = true;
			if (iLimit < 10) iLimit = 0;
			else if (iLimit < 101) { if (iLimit >= 10) iLimit -= 10; }
			else if (iLimit < 501) iLimit -= 50;
			else if (iLimit < 1001) iLimit -= 100;
			else iLimit -= 500;
		}
		else if (!CapSense_CheckIsWidgetActive(CapSense_DOWN__BTN) && downPressed == true)
		{
			downPressed = false;
		}

		if (CapSense_CheckIsWidgetActive(CapSense_UP__BTN) && upPressed == false)
		{
			upPressed = true;
			if (iLimit < 99) iLimit += 10;
			else if (iLimit < 500) iLimit += 50;
			else if (iLimit < 1000) iLimit += 100;
			else if (iLimit < 3500) iLimit += 500;
			else iLimit = 4000;
		}
		else if (!CapSense_CheckIsWidgetActive(CapSense_UP__BTN) && upPressed == true)
		{
			upPressed = false;
		}


		// Fetch any waiting characters from the USB UART.
		int charCount = USBUART_GetCount();
		if (charCount > 0)
		{
			int i = USBUART_GetAll(inBuff);

			for (i = 0; i < charCount; i++)
			{
				floatBuff[incCharIndex++] = inBuff[i];
				if (incCharIndex > 63)
					incCharIndex = 0;
				if (inBuff[i] == '\r' ||
					inBuff[i] == '\n')
				{
					floatBuff[incCharIndex] = '\0';
					break;
				}
			}
		}

		// Average the DMA data from the Source Voltage ADC
		vAve = 0;
		for (i = 0; i < ADCSAMPLES; i++)
			vAve += SourceData[i];

    vAve = vAve/40;
		vSource = 10*Source_ADC_CountsTo_mVolts((uint16)(vAve)); // 10*vAve/40
		if (systemTimer - 20 > SlowTick)
		{
			SlowTick = systemTimer;
			cls();
			goToPos(1, 1);
			putString("I Source:");
			goToPos(1, 2);
			putString("I Limit:");
			goToPos(1, 3);
			putString("V Source:");
			goToPos(1, 4);
			putString("V Min:");
			goToPos(1, 5);
			putString("mA Hours:");
			goToPos(12, 1);
			if (iSource < 0) iSource *= -1;
			sprintf(buff, "%6.3f", iSource / 1000.0f);
			putString(buff);
			goToPos(12, 2);
			sprintf(buff, "%6.3f", iLimit / 1000.0f);
			putString(buff);
			goToPos(12, 3);
			sprintf(buff, "%6.3f", (float)vSource / 1000.0f);
			putString(buff);
			goToPos(12, 4);
			sprintf(buff, "%6.3f", vMin / 1000.0f);
			putString(buff);
			goToPos(12, 5);
			sprintf(buff, "%6.2f", maHours / 3600.0f);
			putString(buff);

			sprintf(buff, "I: %.2f V: %.2f", iLimit / 1000.0f, vSource / 1000.0f);
			LCD_Position(0, 0);
			LCD_PrintString(buff);
			sprintf(buff, "Imeas: %.2f", iSource / 1000.0f);
			LCD_Position(1, 0);
			LCD_PrintString(buff);

			if (floatBuff[incCharIndex] == '\0')
			{
				fiLimit = atoff(floatBuff + 1);
				switch (toupper(floatBuff[0]))
				{
				case 'I':
					if (fiLimit > 4.000 ||
						fiLimit < 0.0) fiLimit = 0.0;
					iLimit = 1000.0*fiLimit;
					break;
				case 'V':
					vMin = 1000.0*fiLimit;
					break;
				case 'E':
					OutputEnable(fiLimit == 1);
					break;
				case 'R':
					maHours = 0;
					break;
        case 'B':
          Bootloadable_Load();
          CySoftwareReset();
          break; // We'll never see this because the previous line resets the
                 //  processor.
				default:
					break;
				}

				memset(floatBuff, 1, 64);
				incCharIndex = 0;
			}
		}
	}
}

void PIDIsr_Interrupt_InterruptCallback()
{
	systemTimer++;
	DoPid();
}


void DoPid()
{
	static int error = 0;
	static int integral = 0;
	static int32_t iSourceRaw = 0;
	static uint16_t grossSetPoint = 0;
	static uint16_t fineSetPoint = 0;
	static int setPoint = 0;
	static int i;
	static int loopCount = 0;

	iSourceRaw = 0;
	for (i = 0; i < 10; i++)
		iSourceRaw += CurrentReadings[i];

	iSource = I_Source_ADC_CountsTo_mVolts(iSourceRaw);

	error = iLimit - iSource;
	integral = integral + error;
	setPoint = (KPN * iLimit) / KPD + (KIN * integral) / KID + 2000; // Use feed forward plus integral
	if (setPoint < 0)
		setPoint = 0;

	// setPoint is a voltage. We need to convert that
	//  into an integer that can be fed into our DACs.
	// First, find our grossSetPoint. This is a largish voltage that
	//  represents a coarse 0-4V offset in 16mV steps.
	grossSetPoint = (int)(setPoint / 16);
	// We want to limit our gross offset to 255, since it's an 8-bit
	//  DAC output.
	if (grossSetPoint > 255)
		grossSetPoint = 255;
	// Now, find the fineSetPoint. This is a 4mV step 8-bit DAC which
	//  allows us to tune the set point a little more finely.
	fineSetPoint = (setPoint - grossSetPoint * 16) / 4;
	if (fineSetPoint > 255)
		fineSetPoint = 255;

	// Finally, one last check: if the source voltage is below vMin,
	//  or the total power is greater than 15W,
	//  disable.
	if ((vSource < vMin) ||
		(vSource * iLimit > 15000000))
		OutputEnable(false);

	if (enableOutput == false)
	{
		error = 0;
		integral = 0;
		grossSetPoint = 0;
		fineSetPoint = 0;
	}
	else
	{
		if (loopCount++ == 100)
		{
			maHours += iSource;
			loopCount = 0;
		}
	}

	Offset_SetValue(grossSetPoint);
	Gate_Drive_Tune_SetValue(fineSetPoint);
}

void OutputEnable(bool v)
{
	enableOutput = v;
	if (enableOutput)
		Output_On_LED_Write(1);
	else
		Output_On_LED_Write(0);
}

