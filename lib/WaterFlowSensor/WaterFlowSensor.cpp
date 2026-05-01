#include "WaterFlowSensor.h"

WaterFlowSensor::WaterFlowSensor(uint8_t pin, float calibrationFactor) 
    : _pin(pin), _calibrationFactor(calibrationFactor), _pulseCount(0), _lastMillis(0), _totalLiters(0.0) {}

void WaterFlowSensor::begin() {
    pinMode(_pin, INPUT_PULLUP);
    // ESP32 specific: Pass 'this' pointer to the ISR
    attachInterruptArg(digitalPinToInterrupt(_pin), handleInterrupt, this, FALLING);
}

// The ISR: simply increments the pulse count
void IRAM_ATTR WaterFlowSensor::handleInterrupt(void* arg) {
    WaterFlowSensor* sensor = static_cast<WaterFlowSensor*>(arg);
    sensor->_pulseCount++;
}

float WaterFlowSensor::getFlowRate() {
    uint32_t now = millis();
    uint32_t duration = now - _lastMillis;

    if (duration == 0) return 0.0;

    // Pulse count is volatile; take a local snapshot to ensure atomicity
    noInterrupts();
    uint32_t pulses = _pulseCount;
    _pulseCount = 0; // Reset for the next interval
    interrupts();

    _lastMillis = now;

    // Formula: (Pulses / Calibration Factor) * (1000ms / duration)
    // For YF-S201, typical F = 7.5 * Q (Q is L/min)
    float flowRate = (static_cast<float>(pulses) / _calibrationFactor) * (1000.0 / duration);
    
    // Track cumulative volume: (flowRate / 60 seconds) * (duration in seconds)
    _totalLiters += (flowRate / 60.0) * (duration / 1000.0);

    return flowRate;
}

float WaterFlowSensor::getTotalVolume() { return _totalLiters; }
void WaterFlowSensor::resetVolume() { _totalLiters = 0; }