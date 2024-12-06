#include "webgpu_renderer.h"
#include "logstorm/manager.h"
#include <array>
#include <set>
#include <string>
#include <vector>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/val.h>
#include <imgui/imgui_impl_wgpu.h>
#include <magic_enum/magic_enum.hpp>
#include "vectorstorm/matrix/matrix4.h"

namespace render {

namespace {

template<typename Tcpp, typename Tc>
std::string enum_wgpu_name(Tc enum_in) {
  /// Attempt to interpret an enum into its most human-readable form, with fallbacks for unknown types
  /// Tc is the C API enum (WGPU...), Tcpp is the C++ API enum equivalent (wgpu::...)
  if(auto enum_out_opt{magic_enum::enum_cast<Tcpp>(enum_in)}; enum_out_opt.has_value()) { // first try to cast it to the C++ enum for clearest output
    return std::string{magic_enum::enum_name(*enum_out_opt)};
  }

  if(auto enum_out_opt{magic_enum::enum_cast<Tc>(enum_in)}; enum_out_opt.has_value()) { // fall back to trying the C enum interpretation
   return std::string{magic_enum::enum_name(*enum_out_opt)} + " (C binding only)";
  }

  std::ostringstream oss;
  oss << "unknown enum 0x" << std::hex << enum_in;                              // otherwise output the hex value and an explanatory note
  return oss.str();
}

template<typename Tcpp, typename Tc>
std::string enum_wgpu_name(Tcpp enum_in) {
  /// When passing the C++ version, cast it to the C version
  return enum_wgpu_name<Tcpp, Tc>(static_cast<Tc>(enum_in));
}

}

webgpu_renderer::webgpu_renderer(logstorm::manager &this_logger)
  : logger{this_logger} {
  /// Construct a WebGPU renderer and populate those members that don't require delayed init
  if(!webgpu.instance) throw std::runtime_error{"Could not initialize WebGPU"};

  // find out about the initial canvas size and the current window and doc sizes
  window.viewport_size.assign(emscripten::val::global("window")["innerWidth"].as<unsigned int>(),
                              emscripten::val::global("window")["innerHeight"].as<unsigned int>());
  logger << "WebGPU: Viewport size: " << window.viewport_size;

  // create a surface
  {
    wgpu::SurfaceDescriptorFromCanvasHTMLSelector surface_descriptor_from_canvas;
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

  {
    // request an adapter
    wgpu::RequestAdapterOptions adapter_request_options{
      .compatibleSurface{webgpu.surface},
      .powerPreference{wgpu::PowerPreference::HighPerformance},
    };

    webgpu.instance.RequestAdapter(
      &adapter_request_options,
      [](WGPURequestAdapterStatus status_c, WGPUAdapterImpl *adapter_ptr, const char *message, void *data){
        /// Request adapter callback
        auto &renderer{*static_cast<webgpu_renderer*>(data)};
        auto &logger{renderer.logger};
        auto &webgpu{renderer.webgpu};
        if(message) logger << "WebGPU: Request adapter callback message: " << message;
        if(auto status{static_cast<wgpu::RequestAdapterStatus>(status_c)}; status != wgpu::RequestAdapterStatus::Success) {
          logger << "ERROR: WebGPU adapter request failure, status " << enum_wgpu_name<wgpu::RequestAdapterStatus>(status_c);
          throw std::runtime_error{"WebGPU: Could not get adapter"};
        }

        auto &adapter{webgpu.adapter};
        adapter = wgpu::Adapter::Acquire(adapter_ptr);
        if(!adapter) throw std::runtime_error{"WebGPU: Could not acquire adapter"};

        webgpu.surface_preferred_format = webgpu.surface.GetPreferredFormat(adapter);
        logger << "WebGPU surface preferred format for this adapter: " << magic_enum::enum_name(webgpu.surface_preferred_format);
        if(webgpu.surface_preferred_format == wgpu::TextureFormat::Undefined) {
          webgpu.surface_preferred_format = wgpu::TextureFormat::BGRA8Unorm;
          logger << "WebGPU manually specifying preferred format: " << magic_enum::enum_name(webgpu.surface_preferred_format);
        }

        {
          wgpu::AdapterInfo adapter_info;
          adapter.GetInfo(&adapter_info);
          logger << "WebGPU adapter info: " << adapter_info.description << " (" << magic_enum::enum_name(adapter_info.backendType) << ", " << adapter_info.vendor << ", " << adapter_info.architecture << ")";
        }
        std::set<wgpu::FeatureName> adapter_features;
        {
          // see https://developer.mozilla.org/en-US/docs/Web/API/GPUSupportedFeatures and https://www.w3.org/TR/webgpu/#feature-index
          auto const count{adapter.EnumerateFeatures(nullptr)};
          logger << "DEBUG: WebGPU adapter features count: " << count;
          std::vector<wgpu::FeatureName> adapter_features_arr(count);
          adapter.EnumerateFeatures(adapter_features_arr.data());
          for(unsigned int i{0}; i != adapter_features_arr.size(); ++i) {
            adapter_features.emplace(adapter_features_arr[i]);
          }
        }
        for(auto const feature : adapter_features) {
          logger << "DEBUG: WebGPU adapter features: " << enum_wgpu_name<wgpu::FeatureName, WGPUFeatureName>(feature);
        }

        wgpu::SupportedLimits adapter_limits;
        bool const result{adapter.GetLimits(&adapter_limits)};
        if(!result) throw std::runtime_error{"WebGPU: Could not query adapter limits"};

        // specify required features for the device
        std::set<wgpu::FeatureName> required_features{
          // requesting nothing in this simple imgui-focused demo
        };
        std::set<wgpu::FeatureName> desired_features{
          // requesting nothing in this simple imgui-focused demo
        };

        std::vector<wgpu::FeatureName> required_features_arr;
        for(auto const feature : required_features) {
          if(!adapter_features.contains(feature)) {
            logger << "WebGPU: Required adapter feature " << magic_enum::enum_name(feature) << " unavailable, cannot continue";
            throw std::runtime_error{"WebGPU: Required adapter feature " + std::string{magic_enum::enum_name(feature)} + " not available"};
          }
          logger << "WebGPU: Required adapter feature: " << magic_enum::enum_name(feature) << " requested";
          required_features_arr.emplace_back(feature);
        }
        for(auto const feature : desired_features) {
          if(!adapter_features.contains(feature)) {
            logger << "WebGPU: Desired adapter feature " << magic_enum::enum_name(feature) << " unavailable, continuing without it";
            continue;
          }
          logger << "WebGPU: Desired adapter feature " << magic_enum::enum_name(feature) << " requested";
          required_features_arr.emplace_back(feature);
        }

        // specify required limits for the device
        struct limit {
          wgpu::Limits required{
            // requesting nothing in this simple imgui-focused demo
          };
          wgpu::Limits desired{
            .maxTextureDimension2D{8192},
          };
        } requested_limits;

        auto require_limit{[&]<typename T>(std::string const &name, T available, T required, T desired){
          constexpr auto undefined{std::numeric_limits<T>::max()};
          if(required == undefined) {                                           // no hard requirement for this value
            if(desired == undefined) {                                          //   no specific desire for this value
              return undefined;                                                 //     we don't care about the value
            } else {                                                            //   we have a desire for a specific value
              if(available == undefined) {                                      //     but it's not available
                logger << "WebGPU: Desired minimum limit for " << name << " is " << desired << " but is unavailable, ignoring";
                return undefined;                                               //       that's fine, we don't care
              } else {                                                          //     some limit is available
                logger << "WebGPU: Desired minimum limit for " << name << " is " << desired << ", requesting " << std::min(desired, available);
                return std::min(desired, available);                            //       we'll accept our desired amount or the limit, whichever is lowest
              }
            }
          } else {                                                              // we have a hard requirement for this value
            if(available == undefined) {                                        //   but it's not available
              logger << "WebGPU: Required minimum limit " << required << " is not available for " << name << " (limit undefined), cannot continue";
              throw std::runtime_error("WebGPU: Required adapter limits not met (limit undefined)");
            } else {                                                            //   some limit is available
              if(available < required) {                                        //     but the limit is below our requirement
                logger << "WebGPU: Required minimum limit " << required << " is not available for " << name << " (max " << available << "), cannot continue";
                throw std::runtime_error("WebGPU: Required adapter limits not met");
              } else {                                                          //     the limit is acceptable
                if(desired == undefined) {                                      //       we have no desire beyond the basic requirement
                  logger << "WebGPU: Required minimum limit for " << name << " is " << required << ", available";
                  return required;                                              //         we'll accept the required minimum
                } else {                                                        //       we desire a value beyond the basic requirement
                  assert(desired > required);                                   //         make sure we're not requesting nonsense with desired values below required minimum
                  logger << "WebGPU: Desired minimum limit for " << name << " is " << desired << ", requesting " << std::min(desired, available);
                  return std::min(desired, available);                          //         we'll accept our desired amount or the limit, whichever is lowest
                }
              }
            }
          }
        }};

        wgpu::RequiredLimits const required_limits{
          .limits{                                                              // see https://www.w3.org/TR/webgpu/#limit-default
            #define REQUIRE_LIMIT(limit) .limit{require_limit(#limit, adapter_limits.limits.limit, requested_limits.required.limit, requested_limits.desired.limit)}
            REQUIRE_LIMIT(maxTextureDimension1D),
            REQUIRE_LIMIT(maxTextureDimension2D),
            REQUIRE_LIMIT(maxTextureDimension3D),
            REQUIRE_LIMIT(maxTextureArrayLayers),
            REQUIRE_LIMIT(maxBindGroups),
            REQUIRE_LIMIT(maxBindGroupsPlusVertexBuffers),
            REQUIRE_LIMIT(maxBindingsPerBindGroup),
            REQUIRE_LIMIT(maxDynamicUniformBuffersPerPipelineLayout),
            REQUIRE_LIMIT(maxDynamicStorageBuffersPerPipelineLayout),
            REQUIRE_LIMIT(maxSampledTexturesPerShaderStage),
            REQUIRE_LIMIT(maxSamplersPerShaderStage),
            REQUIRE_LIMIT(maxStorageBuffersPerShaderStage),
            REQUIRE_LIMIT(maxStorageTexturesPerShaderStage),
            REQUIRE_LIMIT(maxUniformBuffersPerShaderStage),
            REQUIRE_LIMIT(maxUniformBufferBindingSize),
            REQUIRE_LIMIT(maxStorageBufferBindingSize),
            REQUIRE_LIMIT(minUniformBufferOffsetAlignment),
            REQUIRE_LIMIT(minStorageBufferOffsetAlignment),
            // special treatment for minimum rather than maximum limits may be required, see notes for "alignment" at https://www.w3.org/TR/webgpu/#limit-default:
            //.minUniformBufferOffsetAlignment{adapter_limits.limits.minUniformBufferOffsetAlignment},
            //.minStorageBufferOffsetAlignment{adapter_limits.limits.minStorageBufferOffsetAlignment},
            REQUIRE_LIMIT(maxVertexBuffers),
            REQUIRE_LIMIT(maxBufferSize),
            REQUIRE_LIMIT(maxVertexAttributes),
            REQUIRE_LIMIT(maxVertexBufferArrayStride),
            REQUIRE_LIMIT(maxInterStageShaderComponents),
            REQUIRE_LIMIT(maxInterStageShaderVariables),
            REQUIRE_LIMIT(maxColorAttachments),
            REQUIRE_LIMIT(maxColorAttachmentBytesPerSample),
            REQUIRE_LIMIT(maxComputeWorkgroupStorageSize),
            REQUIRE_LIMIT(maxComputeInvocationsPerWorkgroup),
            REQUIRE_LIMIT(maxComputeWorkgroupSizeX),
            REQUIRE_LIMIT(maxComputeWorkgroupSizeY),
            REQUIRE_LIMIT(maxComputeWorkgroupSizeZ),
            REQUIRE_LIMIT(maxComputeWorkgroupsPerDimension),
            #undef REQUIRE_LIMIT
          },
        };

        // request a device
        wgpu::DeviceDescriptor device_descriptor{
          .requiredFeatureCount{required_features_arr.size()},
          .requiredFeatures{required_features_arr.data()},
          .requiredLimits{&required_limits},
          .defaultQueue{
            .label{"Default queue"},
          },
          .deviceLostCallback{[](WGPUDeviceLostReason reason_c, char const *message, void *data){
            /// Device lost callback
            auto &renderer{*static_cast<webgpu_renderer*>(data)};
            auto &logger{renderer.logger};
            logger << "ERROR: WebGPU lost device, reason " << enum_wgpu_name<wgpu::DeviceLostReason>(reason_c) << ": " << message;
          }},
          .deviceLostUserdata{&renderer},
        };

        adapter.RequestDevice(
          &device_descriptor,
          [](WGPURequestDeviceStatus status_c, WGPUDevice device_ptr,  const char *message,  void *data){
            /// Request device callback
            auto &renderer{*static_cast<webgpu_renderer*>(data)};
            auto &logger{renderer.logger};
            auto &webgpu{renderer.webgpu};
            if(message) logger << "WebGPU: Request device callback message: " << message;
            if(auto status{static_cast<wgpu::RequestDeviceStatus>(status_c)}; status != wgpu::RequestDeviceStatus::Success) {
              logger << "ERROR: WebGPU device request failure, status " << enum_wgpu_name<wgpu::RequestDeviceStatus>(status_c);
              throw std::runtime_error{"WebGPU: Could not get adapter"};
            }
            auto &device{webgpu.device};
            device = wgpu::Device::Acquire(device_ptr);

            // report device capabilities
            std::set<wgpu::FeatureName> device_features;
            {
              auto const count{device.EnumerateFeatures(nullptr)};
              std::vector<wgpu::FeatureName> device_features_arr(count);
              device.EnumerateFeatures(device_features_arr.data());
              for(unsigned int i{0}; i != device_features_arr.size(); ++i) {
                device_features.emplace(device_features_arr[i]);
              }
            }
            for(auto const feature : device_features) {
              logger << "DEBUG: WebGPU device features: " << magic_enum::enum_name(feature);
            }

            device.SetUncapturedErrorCallback(
              [](WGPUErrorType type, char const *message, void *data){
                /// Uncaptured error callback
                auto &renderer{*static_cast<webgpu_renderer*>(data)};
                auto &logger{renderer.logger};
                logger << "ERROR: WebGPU uncaptured error " << enum_wgpu_name<wgpu::ErrorType>(type) << ": " << message;
              },
              &renderer
            );
          },
          data
        );
      },
      this
    );
  }

  emscripten_set_main_loop_arg([](void *data){
    /// Dispatch the loop waiting for WebGPU to become ready
    auto &renderer{*static_cast<webgpu_renderer*>(data)};
    renderer.wait_to_configure_loop();
  }, this, 0, true);                                                            // loop function, user data, FPS (0 to use browser requestAnimationFrame mechanism), simulate infinite loop
  std::unreachable();
}

void webgpu_renderer::init_swapchain() {
  /// Create or recreate the swapchain for the current viewport size
  wgpu::SwapChainDescriptor swapchain_descriptor{
    .label{"Swapchain 1"},
    .usage{wgpu::TextureUsage::RenderAttachment},
    .format{webgpu.surface_preferred_format},
    .width{ window.viewport_size.x},
    .height{window.viewport_size.y},
    .presentMode{wgpu::PresentMode::Fifo},
  };
  webgpu.swapchain = webgpu.device.CreateSwapChain(webgpu.surface, &swapchain_descriptor);
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
  {
    wgpu::SurfaceConfiguration surface_configuration{
      .device{webgpu.device},
      .format{webgpu.surface_preferred_format},
      .viewFormats{nullptr},
      .width{ window.viewport_size.x},
      .height{window.viewport_size.y},
    };
    webgpu.surface.Configure(&surface_configuration);
  }

  logger << "WebGPU creating swapchain";
  init_swapchain();

  logger << "WebGPU acquiring queue";
  webgpu.queue = webgpu.device.GetQueue();

  emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false,   // target, userdata, use_capture, callback
    ([](int /*event_type*/, EmscriptenUiEvent const *event, void *data) {       // event_type == EMSCRIPTEN_EVENT_RESIZE
      auto &renderer{*static_cast<webgpu_renderer*>(data)};
      renderer.window.viewport_size.x = static_cast<unsigned int>(event->windowInnerWidth);
      renderer.window.viewport_size.y = static_cast<unsigned int>(event->windowInnerHeight);

      renderer.init_swapchain();
      return true;                                                              // the event was consumed
    })
  );
}

void webgpu_renderer::draw() {
  /// Draw a frame
  {
    wgpu::CommandEncoderDescriptor command_encoder_descriptor{
      .label = "Command encoder 1"
    };
    wgpu::CommandEncoder command_encoder{webgpu.device.CreateCommandEncoder(&command_encoder_descriptor)};

    {
      // set up render pass
      command_encoder.PushDebugGroup("Render pass group 1");
      wgpu::TextureView texture_view{webgpu.swapchain.GetCurrentTextureView()};
      if(!texture_view) throw std::runtime_error{"Could not get current texture view from swap chain"};

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
