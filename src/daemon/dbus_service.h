#pragma once

#include <systemd/sd-bus.h>
#include <sys/eventfd.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

class DbusService {
public:
  DbusService();
  ~DbusService();

  bool Start();
  int GetFd() const;
  int GetNotifyFd() const;
  bool ProcessOnce();
  void FlushEmitQueue(); // main thread only
  void EmitRecognitionResult(const std::string &text);
  void EmitStatusChanged(const std::string &status);

  void SetStartHandler(std::function<void()> handler);
  void SetStartCommandHandler(std::function<void(const std::string &)> handler);
  void SetStopHandler(
      std::function<std::string(const std::string &scene_id)> handler);
  void SetStatusHandler(std::function<std::string()> handler);

  static int handleStartRecording(sd_bus_message *m, void *userdata,
                                  sd_bus_error *error);
  static int handleStartCommandRecording(sd_bus_message *m, void *userdata,
                                         sd_bus_error *error);
  static int handleStopRecording(sd_bus_message *m, void *userdata,
                                 sd_bus_error *error);
  static int handleGetStatus(sd_bus_message *m, void *userdata,
                             sd_bus_error *error);

private:
  sd_bus *bus_ = nullptr;
  sd_bus_slot *slot_ = nullptr;
  int notify_fd_ = -1;

  struct PendingEmit {
    bool is_result;
    std::string payload;
  };
  std::mutex emit_mutex_;
  std::vector<PendingEmit> emit_queue_;

  std::function<void()> start_handler_;
  std::function<void(const std::string &)> start_command_handler_;
  std::function<std::string(const std::string &scene_id)> stop_handler_;
  std::function<std::string()> status_handler_;
};
