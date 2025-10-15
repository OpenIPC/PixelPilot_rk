#ifndef OS_MON_H
#define OS_MON_H

#include <memory>
#include <vector>

class ISensor {
public:
    virtual ~ISensor() = default;
    static void detect(std::vector<std::shared_ptr<ISensor>>);
    virtual void run() = 0;
    virtual bool is_valid() = 0;
};

class OsSensors {
public:
    // Enables CPU load sensor
    void addCPU();
    std::size_t discoverCPU();
    // Adds power supply sensor (currently supported is ina226 via ina2xx kernel driver)
    void addPower(const std::string &sensor_type, const std::string &hwmon_id);
    std::size_t discoverPower();
    // Adds temperature sensor
    void addTemperature(const std::string &thermal_zone);
    std::size_t discoverTemperature();
    // Automatically detects and adds all CPU, power and temperature sensors; returns the number
    // of discovered sensors
    std::size_t autodiscover();

    std::size_t size();

    void run();

private:
    std::vector<std::shared_ptr<ISensor>> sensors;
};

#endif
