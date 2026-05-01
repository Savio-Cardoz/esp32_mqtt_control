#ifndef WATER_FLOW_SENSOR_H
#define WATER_FLOW_SENSOR_H

#include <Arduino.h>

class WaterFlowSensor {
public:
    // Constructor handles pin assignment and basic init
    WaterFlowSensor(uint8_t pin, float calibrationFactor = 7.5);

    // Initialize the interrupt configuration
    void begin();

    // Calculate and return flow rate in L/min
    float getFlowRate();

    // Get total volume passed in Liters
    float getTotalVolume();

    // Reset the total counter
    void resetVolume();

private:
    // ISR needs to be static to be passed to attachInterrupt
    static void IRAM_ATTR handleInterrupt(void* arg);

    uint8_t _pin;
    float _calibrationFactor;
    volatile uint32_t _pulseCount;
    uint32_t _lastMillis;
    float _totalLiters;
};

#endif