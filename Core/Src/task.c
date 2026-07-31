#include "main.h"
#include "cmsis_os.h"

#include <stdio.h>

#define JOYSTICK_ADC_MAX          4095U
#define JOYSTICK_LOW_THRESHOLD    1000U
#define JOYSTICK_HIGH_THRESHOLD   3000U

#define LCD_COLOR_BLACK           0x0000U
#define LCD_COLOR_BLUE            0x001FU
#define LCD_COLOR_GREEN           0x07E0U
#define LCD_COLOR_RED             0xF800U
#define LCD_COLOR_MAGENTA         0xF81FU

extern volatile uint16_t joystickAdcValues[2];

void StartInputTask(void const *argument)
{
  for (;;)
  {
    uint16_t joystickX = JOYSTICK_ADC_MAX - joystickAdcValues[1];
    uint16_t joystickY = joystickAdcValues[0];
    const char *joystickStatus = "CENTER";
    const char *buttonStatus = "NONE";

    if (HAL_GPIO_ReadPin(btnA_GPIO_Port, btnA_Pin) == GPIO_PIN_RESET)
    {
      buttonStatus = "A";
    }
    else if (HAL_GPIO_ReadPin(btnB_GPIO_Port, btnB_Pin) == GPIO_PIN_RESET)
    {
      buttonStatus = "B";
    }
    else if (HAL_GPIO_ReadPin(btnSelect_GPIO_Port, btnSelect_Pin) == GPIO_PIN_RESET)
    {
      buttonStatus = "SELECT";
    }
    else if (HAL_GPIO_ReadPin(btnStart_GPIO_Port, btnStart_Pin) == GPIO_PIN_RESET)
    {
      buttonStatus = "START";
    }

    if (joystickX < JOYSTICK_LOW_THRESHOLD)
    {
      joystickStatus = "LEFT";
    }
    else if (joystickX > JOYSTICK_HIGH_THRESHOLD)
    {
      joystickStatus = "RIGHT";
    }

    if (joystickY < JOYSTICK_LOW_THRESHOLD)
    {
      joystickStatus = "UP";
    }
    else if (joystickY > JOYSTICK_HIGH_THRESHOLD)
    {
      joystickStatus = "DOWN";
    }

    printf("Joystick: %s  Button: %s\n", joystickStatus, buttonStatus);
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
