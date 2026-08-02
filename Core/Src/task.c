#include "main.h"
#include "cmsis_os.h"
#include "gb_rom.h"
#include "peanut_gb.h"

#include <stdio.h>

#define JOYSTICK_ADC_MAX          4095
#define JOYSTICK_LOW_THRESHOLD    1000
#define JOYSTICK_HIGH_THRESHOLD   3000

#define STMPE811_ADDRESS          0x82
#define STMPE811_CHIP_ID          0x0811
#define STMPE811_REG_CHIP_ID      0x00
#define STMPE811_REG_SYS_CTRL1    0x03
#define STMPE811_REG_SYS_CTRL2    0x04
#define STMPE811_REG_INT_STA      0x0B
#define STMPE811_REG_IO_AF        0x17
#define STMPE811_REG_ADC_CTRL1    0x20
#define STMPE811_REG_ADC_CTRL2    0x21
#define STMPE811_REG_TSC_CTRL     0x40
#define STMPE811_REG_TSC_CFG      0x41
#define STMPE811_REG_FIFO_TH      0x4A
#define STMPE811_REG_FIFO_STA     0x4B
#define STMPE811_REG_TSC_FRACT    0x56
#define STMPE811_REG_TSC_DRIVE    0x58

#define CART_RAM_SIZE             (32 * 1024)

#define FRAMEBUFFER_ADDRESS       0xD0000000
#define DISPLAY_WIDTH             240
#define GAME_SCREEN_WIDTH         260
#define GAME_SCREEN_HEIGHT        234
#define GAME_SCREEN_X             3
#define GAME_SCREEN_Y             30
#define GAME_SCALE_NUMERATOR      13
#define GAME_SCALE_DENOMINATOR    8

extern volatile uint16_t joystickAdcValues[2];
extern I2C_HandleTypeDef hi2c3;

typedef enum
{
  SCREEN_ORIENTATION_90,
  SCREEN_ORIENTATION_270
} ScreenOrientation;

static struct gb_s gb;
static uint8_t cartRam[CART_RAM_SIZE];
static volatile uint8_t inputState = 0xFF;
static volatile uint8_t touchPressed;
static volatile ScreenOrientation screenOrientation = SCREEN_ORIENTATION_90;
static ScreenOrientation frameOrientation = SCREEN_ORIENTATION_90;

typedef struct
{
  uint8_t address;
  uint8_t value;
} TouchRegister;

static const uint16_t gamePalette[4] =
{
  0xFFFF,
  0xAD55,
  0x52AA,
  0x0000
};

static const TouchRegister touchInitSequence[] =
{
  {STMPE811_REG_ADC_CTRL2, 0x01U},
  {STMPE811_REG_TSC_CFG, 0x9AU},
  {STMPE811_REG_FIFO_TH, 0x01U},
  {STMPE811_REG_FIFO_STA, 0x01U},
  {STMPE811_REG_FIFO_STA, 0x00U},
  {STMPE811_REG_TSC_FRACT, 0x01U},
  {STMPE811_REG_TSC_DRIVE, 0x01U},
  {STMPE811_REG_TSC_CTRL, 0x73U},
  {STMPE811_REG_INT_STA, 0xFFU}
};

static HAL_StatusTypeDef Touch_ReadRegister(uint8_t address, uint8_t *value)
{
  return HAL_I2C_Mem_Read(&hi2c3, STMPE811_ADDRESS, address, I2C_MEMADD_SIZE_8BIT, value, 1U, 100U);
}

static HAL_StatusTypeDef Touch_WriteRegister(uint8_t address, uint8_t value)
{
  return HAL_I2C_Mem_Write(&hi2c3, STMPE811_ADDRESS, address, I2C_MEMADD_SIZE_8BIT, &value, 1U, 100U);
}

static HAL_StatusTypeDef Touch_Init(void)
{
  uint8_t id[2];

  if (HAL_I2C_Mem_Read(&hi2c3, STMPE811_ADDRESS, STMPE811_REG_CHIP_ID, I2C_MEMADD_SIZE_8BIT, id, sizeof(id), 100U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if ((((uint16_t)id[0] << 8) | id[1]) != STMPE811_CHIP_ID)
  {
    return HAL_ERROR;
  }

  if (Touch_WriteRegister(STMPE811_REG_SYS_CTRL1, 0x02U) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(10U);

  if (Touch_WriteRegister(STMPE811_REG_SYS_CTRL1, 0x00U) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(2U);

  if (Touch_WriteRegister(STMPE811_REG_SYS_CTRL2, 0x08U) != HAL_OK ||
      Touch_WriteRegister(STMPE811_REG_IO_AF, 0x0FU) != HAL_OK ||
      Touch_WriteRegister(STMPE811_REG_ADC_CTRL1, 0x48U) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(2U);

  for (uint32_t i = 0;
       i < sizeof(touchInitSequence) / sizeof(touchInitSequence[0]); ++i)
  {
    if (Touch_WriteRegister(touchInitSequence[i].address, touchInitSequence[i].value) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  HAL_Delay(2U);
  return HAL_OK;
}

static HAL_StatusTypeDef Touch_ReadPressed(uint8_t *pressed)
{
  uint8_t status;

  if (Touch_ReadRegister(STMPE811_REG_TSC_CTRL, &status) != HAL_OK)
  {
    return HAL_ERROR;
  }

  *pressed = (status & 0x80U) != 0U;
  return HAL_OK;
}

static uint8_t GB_ReadRom(struct gb_s *context, const uint_fast32_t address)
{
  (void)context;

  if (address >= gbRomSize)
  {
    return 0xFF;
  }

  return gbRomData[address];
}

static uint8_t GB_ReadCartRam(struct gb_s *context, const uint_fast32_t address)
{
  (void)context;

  if (address >= sizeof(cartRam))
  {
    return 0xFF;
  }

  return cartRam[address];
}

static void GB_WriteCartRam(struct gb_s *context, const uint_fast32_t address, const uint8_t value)
{
  (void)context;

  if (address < sizeof(cartRam))
  {
    cartRam[address] = value;
  }
}

static void GB_Error(struct gb_s *context, const enum gb_error_e error, const uint16_t address)
{
  (void)context;

  printf("GB error %d at 0x%04X\n", (int)error, (unsigned int)address);
  Error_Handler();
}

static void GB_DrawLine(struct gb_s *context, const uint8_t *pixels, const uint_fast8_t line)
{
  volatile uint16_t *framebuffer = (volatile uint16_t *)FRAMEBUFFER_ADDRESS;
  uint32_t scaledYStart = (line * GAME_SCALE_NUMERATOR +
                           GAME_SCALE_DENOMINATOR - 1U) /
                          GAME_SCALE_DENOMINATOR;
  uint32_t scaledYEnd = ((line + 1U) * GAME_SCALE_NUMERATOR +
                         GAME_SCALE_DENOMINATOR - 1U) /
                        GAME_SCALE_DENOMINATOR;
  uint32_t scaledXStart = 0U;

  (void)context;

  for (uint32_t x = 0; x < LCD_WIDTH; ++x)
  {
    uint32_t scaledXEnd = ((x + 1U) * GAME_SCALE_NUMERATOR +
                           GAME_SCALE_DENOMINATOR - 1U) /
                          GAME_SCALE_DENOMINATOR;
    uint16_t color = gamePalette[pixels[x] & LCD_COLOUR];

    for (uint32_t scaledY = scaledYStart; scaledY < scaledYEnd; ++scaledY)
    {
      uint32_t column = frameOrientation == SCREEN_ORIENTATION_90
                      ? GAME_SCREEN_X + GAME_SCREEN_HEIGHT - 1U - scaledY
                      : GAME_SCREEN_X + scaledY;

      for (uint32_t scaledX = scaledXStart;
           scaledX < scaledXEnd; ++scaledX)
      {
        uint32_t row = frameOrientation == SCREEN_ORIENTATION_90
                     ? GAME_SCREEN_Y + scaledX
                     : GAME_SCREEN_Y + GAME_SCREEN_WIDTH - 1U - scaledX;
        framebuffer[row * DISPLAY_WIDTH + column] = color;
      }
    }

    scaledXStart = scaledXEnd;
  }
}

static void GB_Init(void)
{
  enum gb_init_error_e result;
  char romName[17];

  result = gb_init(&gb, GB_ReadRom, GB_ReadCartRam, GB_WriteCartRam, GB_Error, NULL);
  if (result != GB_INIT_NO_ERROR)
  {
    printf("GB init failed: %d\n", (int)result);
    Error_Handler();
  }

  gb_init_lcd(&gb, GB_DrawLine);

  printf("GB init OK\n");
  printf("ROM: %s\n", gb_get_rom_name(&gb, romName));
}

void StartInputTask(void const *argument)
{
  uint8_t touchReady = Touch_Init() == HAL_OK;

  printf("Touch init %s\n", touchReady ? "OK" : "failed");

  for (;;)
  {
    uint16_t joystickX = JOYSTICK_ADC_MAX - joystickAdcValues[1];
    uint16_t joystickY = joystickAdcValues[0];
    uint8_t state = 0xFF;
    uint8_t pressed;

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

    if (touchReady && Touch_ReadPressed(&pressed) == HAL_OK &&
        pressed != touchPressed)
    {
      touchPressed = pressed;
      printf("Touch: %s\n", touchPressed ? "pressed" : "released");

      if (touchPressed)
      {
        screenOrientation = screenOrientation == SCREEN_ORIENTATION_90
                          ? SCREEN_ORIENTATION_270
                          : SCREEN_ORIENTATION_90;
        printf("Screen: %d degrees\n",
               screenOrientation == SCREEN_ORIENTATION_90 ? 90 : 270);
      }
    }

    osDelay(10U);
  }
}

void StartDisplayTask(void const *argument)
{
  GB_Init();

  for (;;)
  {
    gb.direct.joypad = inputState;
    frameOrientation = screenOrientation;
    gb_run_frame(&gb);
  }
}
