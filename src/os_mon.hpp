#ifndef OS_MON_H
#define OS_MON_H

#include <memory>
#include <vector>

class ISensor {
public:
    virtual ~ISensor() = default;
    static void detect(std::vector<std::shared_ptr<ISensor>>& sensors);
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
    // Adds a TCP-listening sensor that receives the air unit's SoC temperature,
    // sent over the wfb-ng tunnel by a small script on the VTX (see openipc-vtx/NOTES.md)
    void addVtxTemp(int port);
    // Automatically detects and adds all CPU, power and temperature sensors; returns the number
    // of discovered sensors
    std::size_t autodiscover();

    std::size_t size();

    void run();

private:
    std::vector<std::shared_ptr<ISensor>> sensors;
};

#endif
