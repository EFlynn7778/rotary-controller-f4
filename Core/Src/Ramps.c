/**
 * Copyright © 2022 <Stefano Bertelli>
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the “Software”), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdlib.h>
#include <math.h>
#include "Ramps.h"
#include "Scales.h"


const osThreadAttr_t taskRampsAttributes = {
.name = "taskRamps",
.stack_size = 128 * 4,
.priority = (osPriority_t) osPriorityNormal,
};

// This variable is the handler for the modbus communication
modbusHandler_t RampsModbusData;


void configureOutputPin(GPIO_TypeDef *Port, uint16_t Pin) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin : PtPin */
  GPIO_InitStruct.Pin = Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(Port, &GPIO_InitStruct);
}


void RampsStart(rampsHandler_t *rampsData) {
    rampsData->shared.servo.ratioDen = 400;
    rampsData->shared.servo.ratioNum = 1;
    rampsData->shared.servo.minSpeed = 1;
    rampsData->shared.servo.maxSpeed = 100;
    rampsData->shared.servo.acceleration = 10;
    rampsData->shared.servo.maxValue = 360;
    rampsData->shared.servo.minValue = 0;

    // Configure Pins
    configureOutputPin(DIR_GPIO_PORT, DIR_PIN);
    configureOutputPin(ENA_GPIO_PORT, ENA_PIN);

    // Initialize and start encoder timer
    for (int j = 0; j < SCALES_COUNT; ++j) {
        initScaleTimer(rampsData->shared.scales[j].timerHandle);
        HAL_TIM_Encoder_Start(rampsData->shared.scales[j].timerHandle, TIM_CHANNEL_ALL);
    }

  // Start synchro interrupt
  HAL_TIM_Base_Start_IT(rampsData->synchroRefreshTimer);

  // Start Modbus
  RampsModbusData.uModbusType = MB_SLAVE;
  RampsModbusData.port = rampsData->modbusUart;
  RampsModbusData.u8id = MODBUS_ADDRESS;
  RampsModbusData.u16timeOut = 1000;
  RampsModbusData.EN_Port = NULL;
  RampsModbusData.u16regs = (uint16_t *) (&rampsData->shared);
  RampsModbusData.u16regsize = sizeof(rampsData->shared) / sizeof(uint16_t);
  RampsModbusData.xTypeHW = USART_HW;
  ModbusInit(&RampsModbusData);
  ModbusStart(&RampsModbusData);

  StartRampsTask(rampsData);
}


/**
 * Call this method from the interrupt service routine associated with the pwm generation timer
 * used to control the stepper shared steps generation
 * @param rampsTimer handle reference to the ramps generation time, the same as the calling isr
 * @param data the data structure holding all the rotary controller data
 */
void MotorPwmTimerISR(rampsHandler_t *data) {
  // Controller is in index mode
  if (data->shared.mode == MODE_SYNCHRO) {
      if (HAL_GPIO_ReadPin(DIR_GPIO_PORT, DIR_PIN) == GPIO_PIN_SET) {
          data->shared.servo.currentSteps++;
      } else {
          data->shared.servo.currentSteps--;
      }
      HAL_TIM_PWM_Stop_IT(data->motorPwmTimer, TIM_CHANNEL_1);
  }
}

double interval = 0.01;

void SynchroRefreshTimerIsr(rampsHandler_t *data) {
    rampsSharedData_t *shared = &(data->shared);

    // Update the scales
    for (int i = 0; i < SCALES_COUNT; i++) {
        shared->scales[i].encoderPrevious = shared->scales[i].encoderCurrent;
        shared->scales[i].encoderCurrent = __HAL_TIM_GET_COUNTER(data->shared.scales[i].timerHandle);
        int32_t distValue, distError;
        distValue = (shared->scales[i].encoderCurrent - shared->scales[i].encoderPrevious) *
                    shared->scales[i].ratioNum /
                    shared->scales[i].ratioDen;
        distError = (shared->scales[i].encoderCurrent - shared->scales[i].encoderPrevious) *
                    shared->scales[i].ratioNum %
                    shared->scales[i].ratioDen;

        shared->scales[i].position += distValue;
        shared->scales[i].error += distError;
        if (shared->scales[i].error >= shared->scales[i].ratioDen) {
            shared->scales[i].position++;
            shared->scales[i].error -= shared->scales[i].ratioDen;
        }
    }

    // start indexing
    shared->servo.indexOffset = (float)(shared->index.reqIndex - shared->index.curIndex)
            * (float)shared->servo.ratioNum
            / (float)shared->servo.ratioDen;

    shared->servo.desiredPosition = shared->servo.indexOffset + shared->servo.absoluteOffset;

    float distanceToGo = fabsf(shared->servo.desiredPosition - shared->servo.currentPosition);
    float time = (shared->servo.currentSpeed - shared->servo.minSpeed) / shared->servo.acceleration;
    float space = (shared->servo.acceleration * time * time ) / 2;

    if (shared->servo.desiredPosition > shared->servo.currentPosition) {
        // Start moving if we need to
        if (shared->servo.currentSpeed == 0) {
            shared->servo.currentSpeed = shared->servo.minSpeed;
        } else if (shared->servo.currentSpeed < shared->servo.maxSpeed && distanceToGo > space) {
            shared->servo.currentSpeed += shared->servo.acceleration * interval;
            if (shared->servo.currentSpeed > shared->servo.maxSpeed) {
                shared->servo.currentSpeed = shared->servo.maxSpeed;
            }
        }
        if (distanceToGo <= space && shared->servo.currentSpeed > shared->servo.minSpeed) {
            shared->servo.currentSpeed -= shared->servo.acceleration;
            if (shared->servo.currentSpeed < shared->servo.minSpeed) {
                shared->servo.currentSpeed = shared->servo.minSpeed;
            }
        }
    }

    if (shared->servo.desiredPosition < shared->servo.currentPosition) {
        if (shared->servo.currentSpeed == 0) {
            shared->servo.currentSpeed = -shared->servo.minSpeed;
        } else if (-shared->servo.currentSpeed < shared->servo.maxSpeed && distanceToGo > space) {
            shared->servo.currentSpeed -= shared->servo.acceleration * interval;
            if (-shared->servo.currentSpeed > shared->servo.maxSpeed) {
                shared->servo.currentSpeed = -shared->servo.maxSpeed;
            }
        }
        if (distanceToGo <= space && -shared->servo.currentSpeed > shared->servo.minSpeed) {
            shared->servo.currentSpeed += shared->servo.acceleration;
            if (-shared->servo.currentSpeed < shared->servo.minSpeed) {
                shared->servo.currentSpeed = -shared->servo.minSpeed;
            }
        }
    }

    shared->servo.desiredSteps = (int32_t)(shared->servo.currentPosition
            * (float)shared->servo.ratioNum
            / (float)shared->servo.ratioDen);

    if (shared->servo.desiredSteps > shared->servo.currentSteps) {
        HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_PIN, GPIO_PIN_SET);
        HAL_TIM_PWM_Start_IT(data->motorPwmTimer, TIM_CHANNEL_1);
    }
    if (shared->servo.desiredSteps < shared->servo.currentSteps) {
        HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_PIN, GPIO_PIN_RESET);
        HAL_TIM_PWM_Start_IT(data->motorPwmTimer, TIM_CHANNEL_1);
    }

}

/**
 * This method is used to initialize the RTOS task responsible for controlling the ramps
 * ramp generator.
 */
void StartRampsTask(rampsHandler_t *rampsData) {
  rampsData->TaskRampsHandle = osThreadNew(RampsTask, rampsData, &taskRampsAttributes);
}

/**
 * This is the FreeRTOS task invoked to handle the general low priority task responsible
 * for the management of all the ramps system operation.
 * @param argument Reference to the ramps handler data structure
 */
void RampsTask(void *argument) {
  rampsHandler_t *data = (rampsHandler_t *) argument;
  rampsSharedData_t *shared = &data->shared;

  uint16_t ledTicks = 0;
  for (;;) {
    osDelay(150);

    // Handle sync mode request
//    if (shared->mode == MODE_SYNCHRO_INIT) {
//      SyncMotionInit(data);
//    }

    // Handle request to set encoder count value
//    if (shared->mode == MODE_SET_ENCODER) {
//      // Reset everything and configure the provided value
//      int scaleIndex = data->shared.encoderPresetIndex;
//      // Counter reset
//      __HAL_TIM_SET_COUNTER(data->scales.scaleTimer[scaleIndex], 0);
//      data->scales.scalePosition[scaleIndex].encoderCurrent = 0;
//      data->scales.scalePosition[scaleIndex].encoderPrevious = 0;
//
//      // Sync data struct reset
//      data->scales.scalePosition[scaleIndex].positionCurrent = shared->encoderPresetValue;
//
//      // Shared data struct reset
//      shared->scales[scaleIndex] = shared->encoderPresetValue;
//      shared->mode = MODE_HALT; // Set proper mode
//    }
  }
}
