#pragma once
#include <expected>
#include <string>
#include <vector>
#include "emscripten_fetch_manager.h"

namespace gui {

class gpt_interface {
  struct message_type {
    enum class roles {
      system,
      user,
      assistant,
    } role{roles::user};
    std::string text{};
  };

  std::string api_key;

  emscripten_fetch_manager fetcher;

  std::expected<std::vector<std::string>, std::string> model_list_result;
  std::vector<std::string const>::iterator model_selected{model_list_result->end()};

  std::vector<message_type> messages{
    {
      .role{message_type::roles::system},
      .text{"You are a helpful assistant..."},
    },
    {
      .role{message_type::roles::user},
    },
  };

public:
  void draw();
};

}
