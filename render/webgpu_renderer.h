#pragma once

#include <webgpu/webgpu_cpp.h>
#include "logstorm/logstorm_forward.h"
#include "vectorstorm/vector/vector2.h"

namespace render {

class webgpu_renderer {
  logstorm::manager &logger;

public:
  struct webgpu_data {
    wgpu::Instance instance{wgpu::CreateInstance()};                            // the underlying WebGPU instance
    wgpu::Surface surface;                                                      // the canvas surface for rendering
    wgpu::Adapter adapter;                                                      // WebGPU adapter once it has been acquired
    wgpu::Device device;                                                        // WebGPU device once it has been acquired
    wgpu::Queue queue;                                                          // the queue for this device, once it has been acquired

    wgpu::SwapChain swapchain;                                                  // the swapchain providing a texture view to render to

    wgpu::TextureFormat surface_preferred_format{wgpu::TextureFormat::Undefined}; // preferred texture format for this surface

  private:
    webgpu_data() = default;
    friend class webgpu_renderer;
  };

private:
  webgpu_data webgpu;

  struct window_data {
    vec2ui viewport_size;                                                       // our idea of the size of the viewport we render to, in real pixels
  } window;

  std::function<void(webgpu_data const&)> postinit_callback;                    // the callback that is called once when init completes (it cannot return normally because of emscripten's loop mechanism)
  std::function<void()> main_loop_callback;                                     // the callback that is called repeatedly for the main loop after init

public:
  webgpu_renderer(logstorm::manager &logger);

  void init(std::function<void(webgpu_data const&)> &&postinit_callback, std::function<void()> &&main_loop_callback);

private:
  void init_swapchain();

  void wait_to_configure_loop();
  void configure();

  void update_imgui_size();

public:
  void draw();
};

}
