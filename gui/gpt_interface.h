#pragma once
#include <string>

namespace gui {

class gpt_interface {
  std::string api_key;

public:
  void draw();
};

}
