#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle);
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
void xTaskNotifyGive(TaskHandle_t task);
