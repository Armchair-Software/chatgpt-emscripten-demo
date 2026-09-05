#include "webgpu_renderer.h"
#include "logstorm/manager.h"
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <imgui/imgui_impl_wgpu.h>
#include <magic_enum/magic_enum.hpp>

namespace render {

namespace {

template<typename Tcpp, typename Tc>
  requires (!std::is_same_v<Tcpp, Tc>)
std::string enum_wgpu_name(Tc enum_in) {
  /// Attempt to interpret an enum into its most human-readable form, with fallbacks for unknown types
  /// Tc is the C API enum (WGPU...), Tcpp is the C++ API enum equivalent (wgpu::...)
  using value_type = std::underlying_type_t<Tc>;
  auto const enum_value{static_cast<value_type>(enum_in)};

  if(auto enum_out_opt{magic_enum::enum_cast<Tcpp>(enum_value)}; enum_out_opt.has_value()) { // first try to cast it to the C++ enum for clearest output
    return std::string{magic_enum::enum_name(*enum_out_opt)};
  }

  if(auto enum_out_opt{magic_enum::enum_cast<Tc>(enum_value)}; enum_out_opt.has_value()) { // fall back to trying the C enum interpretation
   return std::string{magic_enum::enum_name(*enum_out_opt)} + " (C binding only)";
  }

  std::ostringstream oss;
  oss << "unknown enum 0x" << std::hex << enum_value;                           // otherwise output the hex value and an explanatory note
  return oss.str();
}

template<typename Tenum>
std::string enum_wgpu_name(Tenum enum_in) {
  /// Interpret a C++ WebGPU enum directly
  if(auto const enum_name{magic_enum::enum_name(enum_in)}; !enum_name.empty()) {
    return std::string{enum_name};
  }

  std::ostringstream oss;
  oss << "unknown enum 0x" << std::hex << static_cast<std::underlying_type_t<Tenum>>(enum_in);
  return oss.str();
}

std::string_view string_wgpu(wgpu::StringView string_in) {
  /// Convert Dawn string views to standard string views for logging
  return static_cast<std::string_view>(string_in);
}

}

webgpu_renderer::webgpu_renderer(logstorm::manager &this_logger)
  : logger{this_logger} {
  /// Construct a WebGPU renderer and populate those members that don't require delayed init
  if(!webgpu.instance) throw std::runtime_error{"Could not initialize WebGPU"};

  auto const resize_callback{+[](void *data) {
    auto &renderer{*static_cast<webgpu_renderer*>(data)};
    if(!renderer.update_viewport_size() || !renderer.webgpu.queue) return;
    renderer.configure_surface();
  }};
  EM_ASM({
    const canvas = Module["canvas"];
    if(!canvas.__webgpu_device_pixel_resize_observer) {
      const resize_callback = wasmTable.get($0);
      const set_canvas_size = (width, height) => {
        width = Math.max(1, Math.round(width));
        height = Math.max(1, Math.round(height));
        if(canvas.width !== width) canvas.width = width;
        if(canvas.height !== height) canvas.height = height;
      };
      const set_approximate_canvas_size = () => {
        const rect = canvas.getBoundingClientRect();
        set_canvas_size(rect.width * window.devicePixelRatio, rect.height * window.devicePixelRatio);
      };
      set_approximate_canvas_size();
      if(typeof ResizeObserver !== "undefined") {
        const has_device_pixel_content_box = typeof ResizeObserverEntry !== "undefined" && "devicePixelContentBoxSize" in ResizeObserverEntry.prototype;
        const observer = new ResizeObserver((entries) => {
          const entry = entries[0];
          const device_sizes = has_device_pixel_content_box ? entry.devicePixelContentBoxSize : null;
          const device_size = device_sizes && device_sizes.length ? device_sizes[0] : device_sizes;
          if(device_size) set_canvas_size(device_size.inlineSize, device_size.blockSize);
          else set_approximate_canvas_size();
          resize_callback($1);
        });
        observer.observe(canvas, {box: has_device_pixel_content_box ? "device-pixel-content-box" : "content-box"});
        canvas.__webgpu_device_pixel_resize_observer = observer;
        if(!has_device_pixel_content_box) console.warn("ResizeObserver device-pixel-content-box is unavailable; canvas sizing will approximate using devicePixelRatio.");
      } else {
        console.warn("ResizeObserver is unavailable; canvas sizing will approximate using devicePixelRatio.");
        window.addEventListener("resize", () => {
          set_approximate_canvas_size();
          resize_callback($1);
        });
        canvas.__webgpu_device_pixel_resize_observer = true;
      }
    }
  }, resize_callback, this);

  update_viewport_size();
  logger << "WebGPU: Viewport size: " << window.viewport_size;

  // create a surface
  {
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector surface_descriptor_from_canvas;
    surface_descriptor_from_canvas.selector = "#canvas";

    wgpu::SurfaceDescriptor surface_descriptor{
      .nextInChain{&surface_descriptor_from_canvas},
      .label{"Canvas surface"},
    };
    webgpu.surface = webgpu.instance.CreateSurface(&surface_descriptor);
  }
  if(!webgpu.surface) throw std::runtime_error{"Could not create WebGPU surface"};
}

void webgpu_renderer::init(std::function<void(webgpu_data const&)> &&this_postinit_callback, std::function<void()> &&this_main_loop_callback) {
  /// Initialise the WebGPU system
  postinit_callback = this_postinit_callback;
  main_loop_callback = this_main_loop_callback;

  wgpu::RequestAdapterOptions adapter_request_options{
    .powerPreference{wgpu::PowerPreference::HighPerformance},
    .compatibleSurface{webgpu.surface},
  };

  webgpu.instance.RequestAdapter(
    &adapter_request_options,
    wgpu::CallbackMode::AllowSpontaneous,
    [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter_in, wgpu::StringView message, webgpu_renderer *renderer_ptr){
      /// Request adapter callback
      auto &renderer{*renderer_ptr};
      auto &logger{renderer.logger};
      auto &webgpu{renderer.webgpu};
      if(message.length) logger << "WebGPU: Request adapter callback message: " << string_wgpu(message);
      if(status != wgpu::RequestAdapterStatus::Success) {
        logger << "ERROR: WebGPU adapter request failure, status " << enum_wgpu_name(status);
        throw std::runtime_error{"WebGPU: Could not get adapter"};
      }

      auto &adapter{webgpu.adapter};
      adapter = std::move(adapter_in);
      if(!adapter) throw std::runtime_error{"WebGPU: Could not acquire adapter"};

      wgpu::SurfaceCapabilities surface_capabilities;
      webgpu.surface.GetCapabilities(adapter, &surface_capabilities);
      if(surface_capabilities.formatCount != 0) {
        webgpu.surface_preferred_format = surface_capabilities.formats[0];
      }
      logger << "WebGPU surface preferred format for this adapter: " << magic_enum::enum_name(webgpu.surface_preferred_format);
      if(webgpu.surface_preferred_format == wgpu::TextureFormat::Undefined) {
        webgpu.surface_preferred_format = wgpu::TextureFormat::RGBA8Unorm;
        logger << "WebGPU manually specifying preferred format: " << magic_enum::enum_name(webgpu.surface_preferred_format);
      }

      {
        wgpu::AdapterInfo adapter_info;
        adapter.GetInfo(&adapter_info);
        logger << "WebGPU adapter info: " << string_wgpu(adapter_info.description) << " (" << magic_enum::enum_name(adapter_info.backendType) << ", " << string_wgpu(adapter_info.vendor) << ", " << string_wgpu(adapter_info.architecture) << ")";
      }

      std::set<wgpu::FeatureName> adapter_features;
      {
        // see https://developer.mozilla.org/en-US/docs/Web/API/GPUSupportedFeatures and https://www.w3.org/TR/webgpu/#feature-index
        wgpu::SupportedFeatures adapter_supported_features;
        adapter.GetFeatures(&adapter_supported_features);
        logger << "DEBUG: WebGPU adapter features count: " << adapter_supported_features.featureCount;
        for(size_t i{0}; i != adapter_supported_features.featureCount; ++i) {
          adapter_features.emplace(adapter_supported_features.features[i]);
        }
      }
      for(auto const feature : adapter_features) {
        logger << "DEBUG: WebGPU adapter features: " << enum_wgpu_name(feature);
      }

      // This simple ImGui-focused demo needs no optional features or limits above
      // the WebGPU defaults.
      wgpu::DeviceDescriptor device_descriptor;
      device_descriptor.requiredFeatureCount = 0;
      device_descriptor.requiredFeatures = nullptr;
      device_descriptor.requiredLimits = nullptr;
      device_descriptor.defaultQueue.label = "Default queue";
      device_descriptor.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::Device const &, wgpu::DeviceLostReason reason, wgpu::StringView message, webgpu_renderer *renderer_ptr){
          /// Device lost callback
          auto &renderer{*renderer_ptr};
          renderer.logger << "ERROR: WebGPU lost device, reason " << enum_wgpu_name(reason) << ": " << string_wgpu(message);
        },
        &renderer
      );
      device_descriptor.SetUncapturedErrorCallback(
        [](wgpu::Device const &, wgpu::ErrorType type, wgpu::StringView message, webgpu_renderer *renderer_ptr){
          /// Uncaptured error callback
          auto &renderer{*renderer_ptr};
          renderer.logger << "ERROR: WebGPU uncaptured error " << enum_wgpu_name(type) << ": " << string_wgpu(message);
        },
        &renderer
      );

      adapter.RequestDevice(
        &device_descriptor,
        wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestDeviceStatus status, wgpu::Device device_in, wgpu::StringView message, webgpu_renderer *renderer_ptr){
          /// Request device callback
          auto &renderer{*renderer_ptr};
          auto &logger{renderer.logger};
          auto &webgpu{renderer.webgpu};
          if(message.length) logger << "WebGPU: Request device callback message: " << string_wgpu(message);
          if(status != wgpu::RequestDeviceStatus::Success) {
            logger << "ERROR: WebGPU device request failure, status " << enum_wgpu_name(status);
            throw std::runtime_error{"WebGPU: Could not get device"};
          }
          auto &device{webgpu.device};
          device = std::move(device_in);

          // report device capabilities
          std::set<wgpu::FeatureName> device_features;
          {
            wgpu::SupportedFeatures device_supported_features;
            device.GetFeatures(&device_supported_features);
            logger << "DEBUG: WebGPU device features count: " << device_supported_features.featureCount;
            for(size_t i{0}; i != device_supported_features.featureCount; ++i) {
              device_features.emplace(device_supported_features.features[i]);
            }
          }
          for(auto const feature : device_features) {
            logger << "DEBUG: WebGPU device features: " << magic_enum::enum_name(feature);
          }
        },
        &renderer
      );
    },
    this
  );

  emscripten_set_main_loop_arg([](void *data){
    /// Dispatch the loop waiting for WebGPU to become ready
    auto &renderer{*static_cast<webgpu_renderer*>(data)};
    renderer.wait_to_configure_loop();
  }, this, 0, true);                                                            // loop function, user data, FPS (0 to use browser requestAnimationFrame mechanism), simulate infinite loop
  std::unreachable();
}

bool webgpu_renderer::update_viewport_size() {
  /// Refresh the device-pixel framebuffer size, and return whether it has changed
  int framebuffer_width{0};
  int framebuffer_height{0};
  if(emscripten_get_canvas_element_size("#canvas", &framebuffer_width, &framebuffer_height) != EMSCRIPTEN_RESULT_SUCCESS) {
    throw std::runtime_error{"Could not read canvas framebuffer size"};
  }
  vec2ui const new_viewport_size{static_cast<unsigned int>(framebuffer_width), static_cast<unsigned int>(framebuffer_height)};
  if(new_viewport_size.x == 0 || new_viewport_size.y == 0) return false;

  bool const viewport_size_changed{new_viewport_size != window.viewport_size};
  window.viewport_size = new_viewport_size;
  return viewport_size_changed;
}

void webgpu_renderer::configure_surface() {
  /// Create or recreate the configured surface for the current viewport size
  wgpu::SurfaceConfiguration surface_configuration{
    .device{webgpu.device},
    .format{webgpu.surface_preferred_format},
    .usage{wgpu::TextureUsage::RenderAttachment},
    .width{window.viewport_size.x},
    .height{window.viewport_size.y},
    .alphaMode{wgpu::CompositeAlphaMode::Auto},
    .presentMode{wgpu::PresentMode::Fifo},
  };
  webgpu.surface.Configure(&surface_configuration);
}

void webgpu_renderer::wait_to_configure_loop() {
  /// Check if initialisation has completed and the WebGPU system is ready for configuration
  /// Since init occurs asynchronously, some emscripten ticks are needed before this becomes true
  if(!webgpu.device) {
    logger << "WebGPU: Waiting for device to become available";
    // TODO: sensible timeout
    return;
  }
  emscripten_cancel_main_loop();

  configure();

  if(postinit_callback) {
    logger << "WebGPU: Configuration complete, running post-init tasks";
    postinit_callback(webgpu);                                                  // perform any user-provided post-init tasks before launching the main loop
  }

  logger << "WebGPU: Launching main loop";
  emscripten_set_main_loop_arg([](void *data){
    /// Main pseudo-loop waiting for initialisation to complete
    auto &renderer{*static_cast<webgpu_renderer*>(data)};
    renderer.main_loop_callback();
  }, this, 0, true);                                                            // loop function, user data, FPS (0 to use browser requestAnimationFrame mechanism), simulate infinite loop
  std::unreachable();
}

void webgpu_renderer::configure() {
  /// When the device is ready, configure the WebGPU system
  logger << "WebGPU device ready, configuring surface";
  update_viewport_size();
  configure_surface();

  logger << "WebGPU acquiring queue";
  webgpu.queue = webgpu.device.GetQueue();

}

void webgpu_renderer::draw() {
  /// Draw a frame
  wgpu::SurfaceTexture surface_texture;
  webgpu.surface.GetCurrentTexture(&surface_texture);
  if(surface_texture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal
     && surface_texture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    throw std::runtime_error{"Could not get current texture from surface"};
  }

  wgpu::TextureView texture_view{surface_texture.texture.CreateView()};
  if(!texture_view) throw std::runtime_error{"Could not get current texture view from surface"};

  {
    wgpu::CommandEncoderDescriptor command_encoder_descriptor{
      .label = "Command encoder 1"
    };
    wgpu::CommandEncoder command_encoder{webgpu.device.CreateCommandEncoder(&command_encoder_descriptor)};

    {
      // set up render pass
      command_encoder.PushDebugGroup("Render pass group 1");

      wgpu::RenderPassColorAttachment render_pass_colour_attachment{
        .view{texture_view},
        .loadOp{wgpu::LoadOp::Clear},
        .storeOp{wgpu::StoreOp::Store},
        .clearValue{wgpu::Color{0, 0.5, 0.5, 1.0}},
      };
      wgpu::RenderPassDescriptor render_pass_descriptor{
        .label{"Render pass 1"},
        .colorAttachmentCount{1},
        .colorAttachments{&render_pass_colour_attachment},
      };
      wgpu::RenderPassEncoder render_pass_encoder{command_encoder.BeginRenderPass(&render_pass_descriptor)};

      ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), render_pass_encoder.Get()); // render the outstanding GUI draw data

      render_pass_encoder.End();
      command_encoder.PopDebugGroup();
    }

    wgpu::CommandBufferDescriptor command_buffer_descriptor {
      .label = "Command buffer 1"
    };
    wgpu::CommandBuffer command_buffer{command_encoder.Finish(&command_buffer_descriptor)};

    webgpu.queue.Submit(1, &command_buffer);
  }
}

}
