#pragma once
#include <driver/i2c_master.h>
namespace wifichat::wifi {
// FT6336U input only; the UI task owns the bus and receives one release per tap.
class Touch {
  public:
    bool initialize();
    bool released(int &x, int &y);

  private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    bool down_ = false;
    int x_ = 0, y_ = 0;
};
} // namespace wifichat::wifi
