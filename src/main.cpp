#include <lazycom/diagnostics/diagnostics.hpp>
#include <lazycom/serial/libserialport_probe.hpp>
#include <lazycom/ui/tui.hpp>

#include <exception>
#include <iostream>

int main() {
  lazycom::diagnostics::install_terminate_handler();
  try {
    if (!lazycom::serial::libserialport_version_supported()) {
      std::cerr << "LazyCom: unsupported libserialport version\n";
      return 1;
    }
    lazycom::ui::Tui tui;
    return tui.run();
  } catch (const std::exception &exception) {
    std::cerr << "LazyCom fatal: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "LazyCom fatal: unknown exception\n";
    return 1;
  }
}
