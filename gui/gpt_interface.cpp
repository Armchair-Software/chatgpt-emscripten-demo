#include <iostream>
#include <expected>
#include "gpt_interface.h"
#include <emscripten/fetch.h>
#include <imgui/imgui.h>
#include <imgui/imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

using namespace std::string_literals;

namespace gui {

enum class readystate : unsigned short {                                        // from include/emscripten/fetch.h
  unsent,
  opened,
  headers_received,
  loading,
  done,
};
// TODO: move into download or whatever, rename

struct message_type {
  enum class roles {
    system,
    user,
    assistant,
  } role{roles::user};
  std::string text{};
};

struct testobj {
  testobj() {
    std::cout << "DEBUG: testobj constructed" << std::endl;
  }
  ~testobj() {
    std::cout << "DEBUG: testobj destroyed" << std::endl;
  }
};


class emscripten_fetch_manager {
public:
  struct request_params {
    /// Parameters for a fetch request
    std::string method{"GET"};
    std::string const &url{};
    std::vector<std::string> headers{};
    std::string body{};
    std::function<void(unsigned short status, std::string_view data)> on_success; // on_success callback is usually expected
    std::function<void(unsigned short status, std::string_view status_text, std::string_view data)> on_error{};
    uint32_t attributes{EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE}; // using REPLACE without PERSIST_FILE skips querying IndexedDB
  };

  using request_id = uint32_t;

  struct request {
    /// Status of an ongoing request
    std::unique_ptr<std::string const> const data;                              // the body of the request we sent (must be preserved until fetch completes)
    std::function<void(unsigned short status, std::string_view data)> callback_success;
    std::function<void(unsigned short status, std::string_view status_text, std::string_view data)> callback_error;

  public:
    readystate state{readystate::opened};                                       // current state of the request - considered opened as soon as the request is created
    unsigned short status{0};                                                   // HTTP status of the request while in progress (200, 404, etc)
    uint64_t bytes_done{0};
    std::optional<uint64_t> bytes_total{};

    request(std::unique_ptr<std::string const> &&data,
            std::function<void(unsigned short status, std::string_view data)> &&callback_success,
            std::function<void(unsigned short status, std::string_view status_text, std::string_view data)> &&callback_error);
  };
  std::unordered_map<request_id, request> requests;

public:
  request_id fetch(request_params params);
};

emscripten_fetch_manager::request::request(std::unique_ptr<std::string const> &&this_data,
                                           std::function<void(unsigned short status, std::string_view data)> &&this_callback_success,
                                           std::function<void(unsigned short status, std::string_view status_text, std::string_view data)> &&this_callback_error)
  : data(std::move(this_data)),
    callback_success(std::move(this_callback_success)),
    callback_error(std::move(this_callback_error)) {
}

emscripten_fetch_manager::request_id emscripten_fetch_manager::fetch(request_params params) {
  /// Request to download a resource to memory with the specified parameters
  std::vector<const char*> c_headers;
  c_headers.reserve(params.headers.size() + 1);
  for(auto const &header : params.headers) {
    c_headers.emplace_back(header.c_str());                                     // build an array of C strings
  }
  c_headers.emplace_back(nullptr);                                              // terminating null

  auto request_data{std::make_unique<std::string>(std::move(params.body))};

  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  std::strcpy(attr.requestMethod, params.method.c_str());
  attr.attributes = params.attributes;
  attr.requestHeaders = c_headers.data();
  attr.requestData = request_data->data();
  attr.requestDataSize = request_data->size();
  attr.userData = this;
  attr.onsuccess = [](emscripten_fetch_t *fetch){
    /// Success callback
    auto &manager{*static_cast<emscripten_fetch_manager*>(fetch->userData)};
    auto &request{manager.requests.at(fetch->id)};
    request.state = static_cast<readystate>(fetch->readyState);
    request.status = fetch->status;

    request.callback_success(fetch->status, {fetch->data, static_cast<size_t>(fetch->numBytes)});

    manager.requests.erase(fetch->id);
    emscripten_fetch_close(fetch);                                              // free data associated with the fetch
  };
  attr.onerror = [](emscripten_fetch_t *fetch){
    /// Error callback
    auto &manager{*static_cast<emscripten_fetch_manager*>(fetch->userData)};
    auto &request{manager.requests.at(fetch->id)};

    request.state = static_cast<readystate>(fetch->readyState);
    request.status = fetch->status;

    request.callback_error(fetch->status, fetch->statusText, {fetch->data, static_cast<size_t>(fetch->numBytes)});

    manager.requests.erase(fetch->id);
    emscripten_fetch_close(fetch);                                              // also free data on error
  };
  attr.onprogress = [](emscripten_fetch_t *fetch){
    /// Progress updated callback
    // note: enable EMSCRIPTEN_FETCH_STREAM_DATA to populate fetch->data progressively
    auto &manager{*static_cast<emscripten_fetch_manager*>(fetch->userData)};
    auto &request{manager.requests.at(fetch->id)};
    request.state = static_cast<readystate>(fetch->readyState);
    request.status = fetch->status;

    if(fetch->totalBytes == 0) {
      request.bytes_done = fetch->dataOffset + fetch->numBytes;
      request.bytes_total = {};
    } else {
      request.bytes_done = fetch->dataOffset;
      request.bytes_total = fetch->totalBytes;
    }
  };
  attr.onreadystatechange = [](emscripten_fetch_t *fetch){
    /// Download state updated callback
    auto &manager{*static_cast<emscripten_fetch_manager*>(fetch->userData)};
    auto &request{manager.requests.at(fetch->id)};
    request.state = static_cast<readystate>(fetch->readyState);
    request.status = fetch->status;
  };

  auto id{emscripten_fetch(&attr, params.url.c_str())->id};

  requests.emplace(
    std::piecewise_construct,
    std::forward_as_tuple(id),
    std::forward_as_tuple(
      std::move(request_data),
      std::move(params.on_success),
      std::move(params.on_error)
    )
  );
  return id;
}







void gpt_interface::draw() {
  /// Draw the interface window
  if(!ImGui::Begin("Chat")) {
    ImGui::End();
    return;
  }


  static emscripten_fetch_manager fetcher;

  ///static readystate download_state{};
  ///static unsigned short download_status{};
  ///static uint64_t download_bytes_done{0};
  ///static std::optional<uint64_t> download_bytes_total{};

  static std::expected<std::vector<std::string>, std::string> model_list_result;
  static std::vector<std::string const>::iterator model_selected{model_list_result->end()};

  static std::vector<message_type> messages{
    {
      .role{message_type::roles::system},
      .text{"You are a helpful assistant..."},
    },
    {
      .role{message_type::roles::user},
    },
  };

  ImGui::InputText("OpenAI API key", &api_key);
  // TODO: accept paste

  if(ImGui::Button("Request list of models")) {
    model_list_result = {};

    fetcher.fetch({
      .url{"https://api.openai.com/v1/models"},
      .headers{
        "Authorization", "Bearer " + api_key,
      },
      .on_success{[](unsigned short status, std::string_view data){
        model_list_result = {};
        try {
          nlohmann::ordered_json const json = nlohmann::ordered_json::parse(data); // preserve element order when parsing
          for(auto const &model : json.at("data")) {
            model_list_result->emplace_back(model.at("id"));
          }
        } catch(std::exception const &e) {
          model_list_result = std::unexpected{std::string{"Failed to parse model list: "} + e.what()};
        }
        std::sort(model_list_result->begin(), model_list_result->end());
        model_selected = model_list_result->end();
      }},
      .on_error{[](unsigned short status, std::string_view status_text, std::string_view data){
        model_list_result = std::unexpected{std::string{status_text} + ": " + std::string{data}};
      }},
    });
  }

  try {
    auto const &model_list{model_list_result.value()};
    if(!model_list.empty()) {
      if(ImGui::BeginCombo("Model", (model_selected == model_list.end() ? "Select..."s : *model_selected).c_str())) {
        for(auto it{model_list.begin()}; it != model_list.end(); ++it) {
          bool const is_selected{it == model_selected};
          if(ImGui::Selectable(it->c_str(), is_selected)) {
            model_selected = it;
          }
          if(is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }

      if(model_selected != model_list.end()) {
        int imgui_id{0};
        for(auto &message : messages) {
          ImGui::PushID(++imgui_id);
          ImGui::Separator();
          if(ImGui::BeginCombo("Role", std::string{magic_enum::enum_name(message.role)}.c_str())) {
            for(auto const &[this_role, role_name] : magic_enum::enum_entries<message_type::roles>()) {
              bool const is_selected{this_role == message.role};
              if(ImGui::Selectable(std::string{role_name}.c_str(), is_selected)) {
                message.role = this_role;
              }
              if(is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }
          ImGui::InputTextMultiline("Message", &message.text);
          ImGui::PopID();
        }
        if(ImGui::Button("Call")) {
          // TODO: submit button
        }
      }
    }
  } catch(std::bad_expected_access<std::string> const &e) {
    ImGui::TextUnformatted(("Error: Failed to fetch model list: " + e.error()).c_str());
  } catch(std::exception const &e) {
    ImGui::TextUnformatted((std::string{"Error: Exception: "} + e.what()).c_str());
  }

  // TODO: request timeout setting
  // TODO: progress when loading
  // TODO: cancel when loading
  // TODO: error if received

  ImGui::End();
}

}
