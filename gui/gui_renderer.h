#pragma once
#include "logstorm/logstorm_forward.h"
#include "gpt_interface.h"

class ImGui_ImplWGPU_InitInfo;

namespace gui {

class gui_renderer {
  logstorm::manager &logger;

  gpt_interface gpt;

public:
  gui_renderer(logstorm::manager &logger);

  void init(ImGui_ImplWGPU_InitInfo &wgpu_info);

  void draw();
};

}
