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
#define PERFORMANCE_REPORT_MS     1000U

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
static uint8_t gameFrame[LCD_WIDTH * LCD_HEIGHT];
static uint8_t scaledXMap[GAME_SCREEN_WIDTH];
static uint16_t scaledYOffset[GAME_SCREEN_HEIGHT];
static uint32_t drawCycles;

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

static void Performance_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t Performance_CyclesToUs(uint64_t cycles)
{
  return (uint32_t)((cycles * 1000000ULL) / SystemCoreClock);
}

static void GB_WaitForNextFrame(uint32_t frameStart, uint32_t frameTargetCycles)
{
  uint32_t oneMillisecond = SystemCoreClock / 1000U;

  while ((uint32_t)(DWT->CYCCNT - frameStart) + oneMillisecond < frameTargetCycles)
  {
    osDelay(1U);
  }

  while ((uint32_t)(DWT->CYCCNT - frameStart) < frameTargetCycles)
  {
  }
}

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

  for (uint32_t i = 0; i < sizeof(touchInitSequence) / sizeof(touchInitSequence[0]); ++i)
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
  uint32_t startCycles = DWT->CYCCNT;

  (void)context;
  memcpy(&gameFrame[line * LCD_WIDTH], pixels, LCD_WIDTH);
  drawCycles += DWT->CYCCNT - startCycles;
}

static void GB_InitScaleMap(void)
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

static void GB_RenderFrame(void)
{
  volatile uint16_t *framebuffer = (volatile uint16_t *)FRAMEBUFFER_ADDRESS;
  uint32_t startCycles = DWT->CYCCNT;

  if (frameOrientation == SCREEN_ORIENTATION_90)
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

  drawCycles += DWT->CYCCNT - startCycles;
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

  GB_InitScaleMap();
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
  uint64_t totalFrameCycles = 0U;
  uint64_t totalDrawCycles = 0U;
  uint32_t maxFrameCycles = 0U;
  uint32_t frameCount = 0U;
  uint32_t reportStart;
  uint32_t frameTargetCycles;

  Performance_Init();
  GB_Init();
  frameTargetCycles = (uint32_t)(((uint64_t)SystemCoreClock * GB_CYCLES_PER_FRAME) /
                                 GB_CLOCK_FREQUENCY);
  reportStart = HAL_GetTick();

  for (;;)
  {
    uint32_t frameStart;
    uint32_t frameCycles;
    uint32_t now;

    gb.direct.joypad = inputState;
    frameOrientation = screenOrientation;
    drawCycles = 0U;
    frameStart = DWT->CYCCNT;
    gb_run_frame(&gb);
    GB_RenderFrame();
    frameCycles = DWT->CYCCNT - frameStart;

    totalFrameCycles += frameCycles;
    totalDrawCycles += drawCycles;
    ++frameCount;

    if (frameCycles > maxFrameCycles)
    {
      maxFrameCycles = frameCycles;
    }

    GB_WaitForNextFrame(frameStart, frameTargetCycles);
    now = HAL_GetTick();
    if (now - reportStart >= PERFORMANCE_REPORT_MS)
    {
      uint32_t elapsedMs = now - reportStart;
      uint32_t fps10 = frameCount * 10000U / elapsedMs;
      uint32_t averageFrameUs = Performance_CyclesToUs(totalFrameCycles / frameCount);
      uint32_t averageDrawUs = Performance_CyclesToUs(totalDrawCycles / frameCount);
      uint32_t averageCoreUs = averageFrameUs - averageDrawUs;
      uint32_t maxFrameUs = Performance_CyclesToUs(maxFrameCycles);

      printf("Perf: %lu.%lu FPS, frame %lu us, draw %lu us, core %lu us, max %lu us\n",
             fps10 / 10U, fps10 % 10U, averageFrameUs,
             averageDrawUs, averageCoreUs, maxFrameUs);

      totalFrameCycles = 0U;
      totalDrawCycles = 0U;
      maxFrameCycles = 0U;
      frameCount = 0U;
      reportStart = HAL_GetTick();
    }
  }
}
