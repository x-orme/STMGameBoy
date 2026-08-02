#include "main.h"
#include "cmsis_os.h"
#include "gb_rom.h"
#include "peanut_gb.h"

#include <stdio.h>
#include <string.h>

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
#define GB_CLOCK_FREQUENCY        4194304ULL
#define GB_CYCLES_PER_FRAME       70224ULL

extern volatile uint16_t joystickAdcValues[2];
extern I2C_HandleTypeDef hi2c3;

static struct gb_s gb;
static uint8_t cartRam[CART_RAM_SIZE];
static volatile uint8_t inputState = 0xFF;
static uint8_t touchPressed;
static volatile uint8_t screenFlipped;
static uint8_t frameFlipped;
static uint8_t gameFrame[LCD_WIDTH * LCD_HEIGHT];
static uint8_t scaledXMap[GAME_SCREEN_WIDTH];
static uint16_t scaledYOffset[GAME_SCREEN_HEIGHT];

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

static void gbWaitForNextFrame(uint32_t frameStart, uint32_t frameTargetCycles)
{
  uint32_t oneMillisecond = SystemCoreClock / 1000U;

  while ((uint32_t)(DWT->CYCCNT - frameStart) + oneMillisecond < frameTargetCycles)
  {
    osDelay(1U);
  }

  while ((uint32_t)(DWT->CYCCNT - frameStart) < frameTargetCycles)
  {
    ;
  }
}

static HAL_StatusTypeDef touchWriteRegister(uint8_t address, uint8_t value)
{
  return HAL_I2C_Mem_Write(&hi2c3, STMPE811_ADDRESS, address, I2C_MEMADD_SIZE_8BIT, &value, 1U, 100U);
}

static HAL_StatusTypeDef touchInit(void)
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

  if (touchWriteRegister(STMPE811_REG_SYS_CTRL1, 0x02U) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(10U);

  if (touchWriteRegister(STMPE811_REG_SYS_CTRL1, 0x00U) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(2U);

  if (touchWriteRegister(STMPE811_REG_SYS_CTRL2, 0x08U) != HAL_OK ||
      touchWriteRegister(STMPE811_REG_IO_AF, 0x0FU) != HAL_OK ||
      touchWriteRegister(STMPE811_REG_ADC_CTRL1, 0x48U) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(2U);

  for (uint32_t i = 0; i < sizeof(touchInitSequence) / sizeof(touchInitSequence[0]); ++i)
  {
    if (touchWriteRegister(touchInitSequence[i].address, touchInitSequence[i].value) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  HAL_Delay(2U);
  return HAL_OK;
}

static HAL_StatusTypeDef touchReadPressed(uint8_t *pressed)
{
  uint8_t status;

  if (HAL_I2C_Mem_Read(&hi2c3, STMPE811_ADDRESS, STMPE811_REG_TSC_CTRL, I2C_MEMADD_SIZE_8BIT, &status, 1U, 100U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  *pressed = (status & 0x80U) != 0U;
  return HAL_OK;
}

static uint8_t gbReadRom(struct gb_s *context, const uint_fast32_t address)
{
  (void)context;

  if (address >= gbRomSize)
  {
    return 0xFF;
  }

  return gbRomData[address];
}

static uint8_t gbReadCartRam(struct gb_s *context, const uint_fast32_t address)
{
  (void)context;

  if (address >= sizeof(cartRam))
  {
    return 0xFF;
  }

  return cartRam[address];
}

static void gbWriteCartRam(struct gb_s *context, const uint_fast32_t address, const uint8_t value)
{
  (void)context;

  if (address < sizeof(cartRam))
  {
    cartRam[address] = value;
  }
}

static void gbHandleError(struct gb_s *context, const enum gb_error_e error, const uint16_t address)
{
  (void)context;

  printf("GB error %d at 0x%04X\n", (int)error, (unsigned int)address);
  Error_Handler();
}

static void gbDrawLine(struct gb_s *context, const uint8_t *pixels, const uint_fast8_t line)
{
  (void)context;
  memcpy(&gameFrame[line * LCD_WIDTH], pixels, LCD_WIDTH);
}

static void gbInitScaleMap(void)
{
  for (uint32_t x = 0; x < GAME_SCREEN_WIDTH; ++x)
  {
    scaledXMap[x] = (uint8_t)(x * LCD_WIDTH / GAME_SCREEN_WIDTH);
  }

  for (uint32_t y = 0; y < GAME_SCREEN_HEIGHT; ++y)
  {
    uint32_t sourceY = y * LCD_HEIGHT / GAME_SCREEN_HEIGHT;
    scaledYOffset[y] = (uint16_t)(sourceY * LCD_WIDTH);
  }
}

static void gbRenderFrame(void)
{
  volatile uint16_t *framebuffer = (volatile uint16_t *)FRAMEBUFFER_ADDRESS;

  if (!frameFlipped)
  {
    for (uint32_t row = 0; row < GAME_SCREEN_WIDTH; ++row)
    {
      uint32_t sourceX = scaledXMap[row];
      volatile uint16_t *output = &framebuffer[(GAME_SCREEN_Y + row) * DISPLAY_WIDTH + GAME_SCREEN_X];

      for (uint32_t column = 0; column < GAME_SCREEN_HEIGHT; ++column)
      {
        uint32_t sourceOffset = scaledYOffset[GAME_SCREEN_HEIGHT - 1U - column];
        output[column] = gamePalette[gameFrame[sourceOffset + sourceX] & LCD_COLOUR];
      }
    }
  }
  else
  {
    for (uint32_t row = 0; row < GAME_SCREEN_WIDTH; ++row)
    {
      uint32_t sourceX = scaledXMap[GAME_SCREEN_WIDTH - 1U - row];
      volatile uint16_t *output = &framebuffer[(GAME_SCREEN_Y + row) * DISPLAY_WIDTH + GAME_SCREEN_X];

      for (uint32_t column = 0; column < GAME_SCREEN_HEIGHT; ++column)
      {
        uint32_t sourceOffset = scaledYOffset[column];
        output[column] = gamePalette[gameFrame[sourceOffset + sourceX] & LCD_COLOUR];
      }
    }
  }
}

static void gbInit(void)
{
  enum gb_init_error_e result;
  char romName[17];

  result = gb_init(&gb, gbReadRom, gbReadCartRam, gbWriteCartRam, gbHandleError, NULL);
  if (result != GB_INIT_NO_ERROR)
  {
    printf("GB init failed: %d\n", (int)result);
    Error_Handler();
  }

  gbInitScaleMap();
  gb_init_lcd(&gb, gbDrawLine);

  printf("GB init OK\n");
  printf("ROM: %s\n", gb_get_rom_name(&gb, romName));
}

void StartInputTask(void const *argument)
{
  uint8_t touchReady = touchInit() == HAL_OK;

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

    inputState = state;
    // printf("Input: 0x%02X\n", inputState);

    if (touchReady && touchReadPressed(&pressed) == HAL_OK &&
        pressed != touchPressed)
    {
      touchPressed = pressed;
      // printf("Touch: %s\n", touchPressed ? "pressed" : "released");

      if (touchPressed)
      {
        screenFlipped = !screenFlipped;
        // printf("Screen flipped\n");
      }
    }

    osDelay(10U);
  }
}

void StartDisplayTask(void const *argument)
{
  uint32_t frameTargetCycles;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  gbInit();
  frameTargetCycles = (uint32_t)(((uint64_t)SystemCoreClock * GB_CYCLES_PER_FRAME) / GB_CLOCK_FREQUENCY);

  for (;;)
  {
    uint32_t frameStart;

    gb.direct.joypad = inputState;
    frameFlipped = screenFlipped;
    frameStart = DWT->CYCCNT;
    gb_run_frame(&gb);
    gbRenderFrame();

    gbWaitForNextFrame(frameStart, frameTargetCycles);
  }
}
