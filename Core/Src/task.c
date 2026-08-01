#include "main.h"
#include "cmsis_os.h"
#include "peanut_gb.h"

#include <stdio.h>

#define JOYSTICK_ADC_MAX          4095
#define JOYSTICK_LOW_THRESHOLD    1000
#define JOYSTICK_HIGH_THRESHOLD   3000

#define LCD_COLOR_BLACK           0x0000
#define LCD_COLOR_BLUE            0x001F
#define LCD_COLOR_GREEN           0x07E0
#define LCD_COLOR_RED             0xF800
#define LCD_COLOR_MAGENTA         0xF81F

extern volatile uint16_t joystickAdcValues[2];
static volatile uint8_t inputState = 0xFF;

void StartInputTask(void const *argument)
{
  for (;;)
  {
    uint16_t joystickX = JOYSTICK_ADC_MAX - joystickAdcValues[1];
    uint16_t joystickY = joystickAdcValues[0];
    uint8_t state = 0xFF;

    if (HAL_GPIO_ReadPin(btnA_GPIO_Port, btnA_Pin) == GPIO_PIN_RESET)
    {
      state &= (uint8_t)~JOYPAD_A;
    }

    if (HAL_GPIO_ReadPin(btnB_GPIO_Port, btnB_Pin) == GPIO_PIN_RESET)
    {
      state &= (uint8_t)~JOYPAD_B;
    }

    if (HAL_GPIO_ReadPin(btnSelect_GPIO_Port, btnSelect_Pin) == GPIO_PIN_RESET)
    {
      state &= (uint8_t)~JOYPAD_SELECT;
    }

    if (HAL_GPIO_ReadPin(btnStart_GPIO_Port, btnStart_Pin) == GPIO_PIN_RESET)
    {
      state &= (uint8_t)~JOYPAD_START;
    }

    if (joystickX < JOYSTICK_LOW_THRESHOLD)
    {
      state &= (uint8_t)~JOYPAD_LEFT;
    }
    else if (joystickX > JOYSTICK_HIGH_THRESHOLD)
    {
      state &= (uint8_t)~JOYPAD_RIGHT;
    }

    if (joystickY < JOYSTICK_LOW_THRESHOLD)
    {
      state &= (uint8_t)~JOYPAD_UP;
    }
    else if (joystickY > JOYSTICK_HIGH_THRESHOLD)
    {
      state &= (uint8_t)~JOYPAD_DOWN;
    }

    if (state != inputState)
    {
      inputState = state;
      printf("Input: 0x%02X\n", inputState);
    }

    osDelay(10U);
  }
}

void StartDisplayTask(void const *argument)
{
  static const uint16_t colors[] =
  {
    LCD_COLOR_RED,
    LCD_COLOR_GREEN,
    LCD_COLOR_BLUE,
    LCD_COLOR_MAGENTA,
    LCD_COLOR_BLACK
  };
  uint32_t colorIndex = 0U;

  for (;;)
  {
    LCD_Fill(colors[colorIndex]);
    colorIndex = (colorIndex + 1U) % (sizeof(colors) / sizeof(colors[0]));
    osDelay(1000U);
  }
}
