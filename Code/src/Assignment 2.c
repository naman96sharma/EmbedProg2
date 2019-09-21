#include "lpc17xx_pinsel.h"
#include "lpc17xx_gpio.h"
#include "lpc17xx_i2c.h"
#include "lpc17xx_ssp.h"
#include "lpc17xx_timer.h"
#include "lpc17xx_uart.h"

#include "pca9532.h"
#include "acc.h"
#include "oled.h"
#include "rgb.h"
#include "light.h"
#include "led7seg.h"
#include "temp.h"
#include "stdio.h"

#define LIGHTNING_MONITORING 3000

int Light_Threshold = 3000; //Starting value for Light_Threshold
volatile uint32_t msTicks; // counter for 1ms SysTicks
volatile uint32_t LightStart = 0; //time when light reaches 3000 lux
volatile uint32_t LightStop = 0; //time when light reduces from 3000 lux
int RecentLightning [9]={0};//Holds the msTicks value for the last 9 instances of lightning
int survival = 0;
int InstantData=0;//Set to 1 when the SW3 interrupt is activated

char UART_Msg[35];//Message to be sent
char UART_LightMsg[66];//Message for Lightning detection

//Values for Accelerometer, Temp, Light and OLED
int32_t xoff = 0;
int32_t yoff = 0;
int32_t zoff = 0;
int8_t x = 0;
int8_t y = 0;
int8_t z = 0;
int32_t temperature;
int32_t light_val;
char TempPrint[16];
char LightPrint[19];
char AccX[15];
char AccY[15];
char AccZ[15];

//The TIMER0 Handler is used to change the LED status at a precise 1 second interval
//Blue LED for Explorer Mode and Red LED for Survival Mode
void TIMER0_IRQHandler (void)
{
	//Determine if TIMER0 Interrupt has occurred
	if((LPC_TIM0->IR & 0x01) == 0x01)
	{
		LPC_TIM0->IR |= 1<<0; //Disable Interrupt
		//Toggle the RGB LED
		if(survival)
		{
			int ledstate = GPIO_ReadValue(2);
			GPIO_ClearValue(2,(ledstate & (1<<0))); //Turn off if on
			GPIO_SetValue(2, ((~ledstate) & (1<<0)));
		}
		else
		{
			int ledstate = GPIO_ReadValue(0);
			GPIO_ClearValue(0,(ledstate & (1<<26))); //Turn off if on
			GPIO_SetValue(0, ((~ledstate) & (1<<26)));
		}
	}
}

//Checks the RecentLightning array and updates 7-Seg accordingly to
//show number of Lightning_Threshold passed in last 3 seconds
void update7Seg(void)
{
	int i, LightNumber = 0;
	for(i=0; i<9; i++)
	{
		if(msTicks-RecentLightning[i]<=3000)
			LightNumber++;
	}
	if(LightNumber==0)
		led7seg_setChar('*', FALSE);//clear 7-seg
	else
		led7seg_setChar('0'+LightNumber, FALSE);
	return;
}

//This function inserts the time of the last Lightning into the first element of array
//Therefore, the array RecentLightning contains the timing of last 9 instances of Lightning
void updateArray (void)
{
	int i;
	for (i=8; i>=1; i--)
		RecentLightning[i] = RecentLightning[i-1];
	RecentLightning[0] = LightStop;
	return;
}

//EINT3 Interrupt Handler
void EINT3_IRQHandler(void)
{
	// Determine whether GPIO Interrupt P2.10 has occurred (SW3)
	if ((LPC_GPIOINT->IO2IntStatF>>10)& 0x1)
	{
		LPC_GPIOINT->IO2IntClr = 1<<10; //Disable Interrupt
		InstantData = 1;
	}
	//Determine whether GPIO Interrupt P2.5 has occurred (Light Sensor)
	if ((LPC_GPIOINT->IO2IntStatF>>5) & 0x1)
	{
		if (Light_Threshold == 3000) //The start of the Lightning pulse
		{
			LightStart = msTicks;
			Light_Threshold = 62271;
			light_setHiThreshold(Light_Threshold);
			light_setLoThreshold(3000);
		}
		else //The "second" interrupt when it detects the light intensity go below 3000 lux
		{
			LightStop = msTicks;
			if ((LightStop - LightStart) <= 500) //Comparing two times to check if the pulse lasted for 500 milliseconds
			{
				updateArray();
				if(msTicks-RecentLightning[2]<=3000)
				{
					survival = 1;
				}
			}
			Light_Threshold = 3000;
			light_setHiThreshold(Light_Threshold);
			light_setLoThreshold(0);
		}
		LPC_GPIOINT->IO2IntClr = (1<<5); //Disable interrupt
		light_clearIrqStatus(); //Disable interrupt from harware
		NVIC_ClearPendingIRQ(EINT3_IRQn);
	}
}

void SysTick_Handler(void) {
  msTicks++;
}

__INLINE static void systick_delay (uint32_t delayTicks) {
  uint32_t currentTicks;
  currentTicks = msTicks;
  while ((msTicks - currentTicks) < delayTicks);
}

uint32_t getMsTicks ()
{ return msTicks;}

static void init_ssp(void)
{
	SSP_CFG_Type SSP_ConfigStruct;
	PINSEL_CFG_Type PinCfg;

	/*
	 * Initialize SPI pin connect
	 * P0.7 - SCK;
	 * P0.8 - MISO
	 * P0.9 - MOSI
	 * P2.2 - SSEL - used as GPIO
	 */
	PinCfg.Funcnum = 2;
	PinCfg.OpenDrain = 0;
	PinCfg.Pinmode = 0;
	PinCfg.Portnum = 0;
	PinCfg.Pinnum = 7;
	PINSEL_ConfigPin(&PinCfg);
	PinCfg.Pinnum = 8;
	PINSEL_ConfigPin(&PinCfg);
	PinCfg.Pinnum = 9;
	PINSEL_ConfigPin(&PinCfg);
	PinCfg.Funcnum = 0;
	PinCfg.Portnum = 2;
	PinCfg.Pinnum = 2;
	PINSEL_ConfigPin(&PinCfg);

	SSP_ConfigStructInit(&SSP_ConfigStruct);

	// Initialize SSP peripheral with parameter given in structure above
	SSP_Init(LPC_SSP1, &SSP_ConfigStruct);

	// Enable SSP peripheral
	SSP_Cmd(LPC_SSP1, ENABLE);

}

static void init_i2c(void)
{
	PINSEL_CFG_Type PinCfg;

	/* Initialize I2C2 pin connect */
	PinCfg.Funcnum = 2;
	PinCfg.Pinnum = 10;
	PinCfg.Portnum = 0;
	PINSEL_ConfigPin(&PinCfg);
	PinCfg.Pinnum = 11;
	PINSEL_ConfigPin(&PinCfg);

	// Initialize I2C peripheral
	I2C_Init(LPC_I2C2, 100000);

	/* Enable I2C1 operation */
	I2C_Cmd(LPC_I2C2, ENABLE);
}
void pinsel_uart3(void)
{
	PINSEL_CFG_Type PinCfg;
	PinCfg.Funcnum = 2;
	PinCfg.Pinnum = 0;
	PinCfg.Portnum = 0;
	PINSEL_ConfigPin(&PinCfg);
	PinCfg.Pinnum = 1;
	PINSEL_ConfigPin(&PinCfg);
}

void init_uart(void)
{
	UART_CFG_Type uartCfg;
	uartCfg.Baud_rate = 115200;
	uartCfg.Databits = UART_DATABIT_8;
	uartCfg.Parity =UART_PARITY_NONE;
	uartCfg.Stopbits = UART_STOPBIT_1;

	pinsel_uart3();

	UART_Init(LPC_UART3, &uartCfg);
	UART_TxCmd(LPC_UART3,ENABLE);
}

static void init_GPIO(void)
{
	// Initialize button
	PINSEL_CFG_Type PinCfg;

		/* Initialize RGB_LEG pin connect */
		PinCfg.Funcnum = 0;
		PinCfg.Pinnum = 26;
		PinCfg.Portnum = 0;
		PINSEL_ConfigPin(&PinCfg);
		PinCfg.Portnum = 2;
		PinCfg.Pinnum = 0;
		PINSEL_ConfigPin(&PinCfg);
		PinCfg.Pinnum = 1;
		PINSEL_ConfigPin(&PinCfg);
		GPIO_SetDir(0,(1<<26),1);
		GPIO_SetDir(2,(1<<0),1);
		GPIO_SetDir(2,(1<<1),1);

		//Initialize temperature sensor
		PinCfg.Funcnum = 0;
		PinCfg.Portnum = 0;
		PinCfg.Pinnum = 2;
		PINSEL_ConfigPin(&PinCfg);
		GPIO_SetDir(0,(1<<2),0);

		//Initialize interrupt pin for Light sensor
		PinCfg.Funcnum = 0;
		PinCfg.Portnum = 2;
		PinCfg.Pinnum = 5;
		PinCfg.Pinmode =0; //Pull Up register
		PINSEL_ConfigPin(&PinCfg);
		GPIO_SetDir(2, (1<<5), 0);

		//Initialize SW4
		PinCfg.Funcnum = 0;
		PinCfg.Portnum = 2;
		PinCfg.Pinnum = 10;
		PinCfg.Pinmode = 0;
		PINSEL_ConfigPin(&PinCfg);
		GPIO_SetDir(2, (1<<10), 0);
}

//The Self Diagnostic code that is done every time HOPE starts fresh
void SelfDiagnostic_Start (void)
{
	 if (SysTick_Config(SystemCoreClock / 1000)) {
	    	    while (1);  // Capture error
	    	}

    init_i2c();
    init_ssp();
    init_GPIO();
    init_uart();

    //Setup Interrupt Priority
    NVIC_SetPriorityGrouping(4);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(TIMER0_IRQn, 0x04);
    NVIC_SetPriority(EINT3_IRQn, 0x08);
    NVIC_SetPriority(UART3_IRQn, 0x0C);

    acc_init();
    oled_init();
    led7seg_init();
    temp_init(&getMsTicks);
    light_enable();
    light_setRange(LIGHT_RANGE_64000); //Allow the sensor to detect 16000 lux
    light_setHiThreshold(Light_Threshold);
    light_setIrqInCycles(LIGHT_CYCLE_1);
    light_clearIrqStatus();
    LPC_GPIOINT->IO2IntEnF |= 1<<5;

    oled_clearScreen(OLED_COLOR_BLACK);

    acc_read(&x, &y, &z);
    xoff = 0-x;
    yoff = 0-y;
    LPC_GPIOINT->IO2IntEnF |= 1<<10; //Enable interrupt from SW3
	NVIC_EnableIRQ(EINT3_IRQn);
    int horizontal = 16;
    int vertical = 12;

    int i;
    for(i=0; i<13; i++)
    {
    	if(i<=6)
    	{
    		led7seg_setChar(('0'+i), FALSE);
				if(InstantData)
				{
					InstantData = 0;
					temperature = temp_read();
					light_val = light_read();
					acc_read(&x, &y, &z);
					x += xoff;
					y += yoff;
					z += zoff;
					sprintf(TempPrint, "Temp: %.2f C   ", temperature/10.0);
					sprintf(LightPrint, "Light: %ld lux   ", light_val);
					sprintf(AccX, "Acc X: %d  ", x);
					sprintf(AccY, "Acc Y: %d  ", y);
					sprintf(AccZ, "Acc Z: %d  ", z);
					oled_putString(0,0, (uint8_t *)TempPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,10, (uint8_t *)LightPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,20, (uint8_t *)AccX, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,30, (uint8_t *)AccY, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,40, (uint8_t *)AccZ, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					sprintf(UART_Msg, "L%d_T%.1f_AX%d_AY%d_AZ%d\n", light_val, temperature/10.0, x, y, z);
					UART_Send(LPC_UART3,(uint8_t *)UART_Msg, strlen(UART_Msg),BLOCKING);
					oled_clearScreen(OLED_COLOR_BLACK);
				}
    		systick_delay(1000);
    	}
    	else
    	{
    		led7seg_setChar(('A'+(i-7)), FALSE);
				if(InstantData)
				{
					InstantData = 0;
					temperature = temp_read();
					light_val = light_read();
					acc_read(&x, &y, &z);
					x += xoff;
					y += yoff;
					z += zoff;
					sprintf(TempPrint, "Temp: %.2f C   ", temperature/10.0);
					sprintf(LightPrint, "Light: %ld lux   ", light_val);
					sprintf(AccX, "Acc X: %d  ", x);
					sprintf(AccY, "Acc Y: %d  ", y);
					sprintf(AccZ, "Acc Z: %d  ", z);
					oled_putString(0,0, (uint8_t *)TempPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,10, (uint8_t *)LightPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,20, (uint8_t *)AccX, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,30, (uint8_t *)AccY, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					oled_putString(0,40, (uint8_t *)AccZ, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
					sprintf(UART_Msg, "L%d_T%.1f_AX%d_AY%d_AZ%d\n", light_val, temperature/10.0, x, y, z);
					UART_Send(LPC_UART3,(uint8_t *)UART_Msg, strlen(UART_Msg),BLOCKING);
					oled_clearScreen(OLED_COLOR_BLACK);
				}
			oled_circle(horizontal, vertical, 10, OLED_COLOR_WHITE);
			oled_circle(horizontal, (64-vertical), 10 ,OLED_COLOR_WHITE);
			horizontal = horizontal + 16;
			vertical = vertical + 10;
    		systick_delay(1000);
    	}
    }
    led7seg_setChar('*', FALSE); //Unrecognized char, 7-seg off
    //Timer0 initialization
    LPC_SC->PCONP |= 1 << 1; //Power on Timer0
    LPC_SC->PCLKSEL0 |= 1 << 2; //Timer0 frequency = CCLK
    LPC_TIM0->MR0 = 100000000; //MR0 is achieved in 1 second
    LPC_TIM0->MCR |= 1 << 0; //Interrupt when TIM0=MR0
    LPC_TIM0->MCR |= 1 << 1; //Reset Timer0 when TIM0=MR0
    NVIC_EnableIRQ(TIMER0_IRQn);
    LPC_TIM0->TCR |= 1 << 0; //Start Timer0
    oled_clearScreen(OLED_COLOR_BLACK);
    return;
}

int main (void)
{
	SelfDiagnostic_Start();
	GPIO_SetValue(0,(1<<26));
	uint32_t currentTicks = 0;

	while(1)
	{
		update7Seg();
		if (survival)
		{
			GPIO_ClearValue(0,(1<<26));
			GPIO_SetValue(2,(1<<0));
			sprintf(TempPrint, "Temp: S      ");
			sprintf(LightPrint, "Light: S       ");
			sprintf(AccX, "Acc X: S  ");
			sprintf(AccY, "Acc Y: S  ");
			sprintf(AccZ, "Acc Z: S  ");
			sprintf(UART_LightMsg, "Lightning Detected. Scheduled Telemetry is Temporarily Suspended.\n");
			UART_Send(LPC_UART3,(uint8_t *)UART_LightMsg, strlen(UART_LightMsg),BLOCKING);
			oled_putString(0,0, (uint8_t *)TempPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
			oled_putString(0,10, (uint8_t *)LightPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
			oled_putString(0,20, (uint8_t *)AccX, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
			oled_putString(0,30, (uint8_t *)AccY, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
			oled_putString(0,40, (uint8_t *)AccZ, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
			uint16_t CountExit=0xffff;
			uint32_t TimeUnit = msTicks;
			for(CountExit=0xffff; CountExit>0; CountExit=(CountExit/2))
			{
				if(light_read()>LIGHTNING_MONITORING)
					CountExit = 0xffff;
				update7Seg();
				if(InstantData)//If SW3 interrupt is set, go to explorer mode for only one cycle and then come back to Survival
				{
					InstantData=0;
					CountExit = CountExit/8; //Reading of values approximately takes 750ms. Hence, 3LEDs switch off
					goto READ_VALUES;
				}
				BACKTOSURVIVAL: pca9532_setLeds(CountExit, 0xffff);
				while((msTicks-TimeUnit)<250);
				TimeUnit = msTicks;
			}
			while((msTicks-TimeUnit)<250);
			CountExit = 0x0000;
			pca9532_setLeds(CountExit, 0xffff);
			GPIO_ClearValue(2,(1<<0));
			GPIO_SetValue(0,(1<<26));
			sprintf(UART_LightMsg, "Lightning Has Subsided. Scheduled Telemetry Will Now Resume.\n");
			UART_Send(LPC_UART3,(uint8_t *)UART_LightMsg, strlen(UART_LightMsg),BLOCKING);
			survival = 0;
		}
		else //Explorer Mode
		{
			if(InstantData)//If interrupt from SW3 is set, do not wait to read values again
			{
				InstantData = 0;
				goto READ_VALUES;
			}
			if(msTicks - currentTicks >2000)
			{
				READ_VALUES: temperature = temp_read();
				light_val = light_read();
				acc_read(&x, &y, &z);
				x += xoff;
				y += yoff;
				z += zoff;
				sprintf(TempPrint, "Temp: %.2f C   ", temperature/10.0);
				sprintf(LightPrint, "Light: %ld lux   ", light_val);
				sprintf(AccX, "Acc X: %d  ", x);
				sprintf(AccY, "Acc Y: %d  ", y);
				sprintf(AccZ, "Acc Z: %d  ", z);
				oled_putString(0,0, (uint8_t *)TempPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
				oled_putString(0,10, (uint8_t *)LightPrint, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
				oled_putString(0,20, (uint8_t *)AccX, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
				oled_putString(0,30, (uint8_t *)AccY, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
				oled_putString(0,40, (uint8_t *)AccZ, OLED_COLOR_WHITE, OLED_COLOR_BLACK);
				sprintf(UART_Msg, "L%d_T%.1f_AX%d_AY%d_AZ%d\n", light_val, temperature/10.0, x, y, z);
				UART_Send(LPC_UART3,(uint8_t *)UART_Msg, strlen(UART_Msg),BLOCKING);
				if(survival)//Go back to survival mode if the SW3 interrupted int eh middle of Survival Mode
					goto BACKTOSURVIVAL;
				currentTicks = msTicks;
			}
		}
	}
	return 0;
}

void check_failed(uint8_t *file, uint32_t line)
{
	while(1);
}
