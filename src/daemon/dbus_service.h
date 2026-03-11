#pragma once

#include <systemd/sd-bus.h>

#include <functional>
#include <mutex>
#include <string>

class DbusService {
public:
    DbusService();
    ~DbusService();

    bool Start();
    int GetFd() const;
    bool ProcessOnce();
    void EmitRecognitionResult(const std::string& text);
    void EmitStatusChanged(const std::string& status);

    void SetStartHandler(std::function<void()> handler);
    void SetStopHandler(
        std::function<std::string(const std::string& scene_id)> handler);
    void SetStatusHandler(std::function<std::string()> handler);

    static int handleStartRecording(sd_bus_message* m, void* userdata,
                                     sd_bus_error* error);
    static int handleStopRecording(sd_bus_message* m, void* userdata,
                                    sd_bus_error* error);
    static int handleGetStatus(sd_bus_message* m, void* userdata,
                                sd_bus_error* error);

private:
    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_ = nullptr;
    std::recursive_mutex bus_mutex_;
    std::function<void()> start_handler_;
    std::function<std::string(const std::string& scene_id)> stop_handler_;
    std::function<std::string()> status_handler_;
};
