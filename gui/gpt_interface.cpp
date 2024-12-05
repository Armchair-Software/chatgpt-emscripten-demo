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

struct message_type {
  enum class roles {
    system,
    user,
    assistant,
  } role{roles::user};
  std::string text{};
};

void gpt_interface::draw() {
  /// Draw the interface window
  if(!ImGui::Begin("Chat")) {
    ImGui::End();
    return;
  }

  static readystate download_state{};
  static unsigned short download_status{};
  static uint64_t download_bytes_done{0};
  static std::optional<uint64_t> download_bytes_total{};

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

  ImGui::TextUnformatted(("Download state: " + std::string{magic_enum::enum_name(download_state)}).c_str());
  if(download_bytes_total.has_value()) {                                        // we have a total, so display percent
    ImGui::TextUnformatted(("Download amount: " + std::to_string(download_bytes_done * 100 / *download_bytes_total) + "%").c_str());
  } else {                                                                      // total unknown, so can't display a percent
    ImGui::TextUnformatted(("Download amount: " + std::to_string(download_bytes_done) + " bytes").c_str());
  }
  ImGui::TextUnformatted(("Last download status: " + std::to_string(download_status)).c_str());

  if(ImGui::Button("Request list of models")) {
    model_list_result = {};
    download_state = readystate::opened;

    std::vector<std::string> headers{
      "Authorization", "Bearer " + api_key,
    };

    std::vector<const char*> c_headers;
    c_headers.reserve(headers.size() + 1);
    for(auto const &header : headers) {
      c_headers.emplace_back(header.c_str());                                   // build an array of C strings
    }
    c_headers.emplace_back(nullptr);                                            // terminating null

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE; // using REPLACE without PERSIST_FILE skips querying IndexedDB
    attr.requestHeaders = c_headers.data();
    ///attr.requestData = nullptr;
    attr.userData = this;
    attr.onsuccess = [](emscripten_fetch_t *fetch){
      // TODO: free requestData here
      std::cout << "DEBUG onsuccess start" << std::endl;
      std::cout << "DEBUG onsuccess: id " << fetch->id << std::endl;
      std::cout << "DEBUG onsuccess: userData " << fetch->userData << std::endl;
      std::cout << "DEBUG onsuccess: url " << (fetch->url ? fetch->url : "null") << std::endl;
      //std::cout << "DEBUG onsuccess: data " << (fetch->data ? fetch->data : "null") << std::endl;
      //std::cout << "DEBUG onsuccess: numBytes " << fetch->numBytes << std::endl;
      std::cout << "DEBUG onsuccess: dataOffset " << fetch->dataOffset << std::endl;
      std::cout << "DEBUG onsuccess: totalBytes " << fetch->totalBytes << std::endl;
      std::cout << "DEBUG onsuccess: readyState " << magic_enum::enum_name(static_cast<readystate>(fetch->readyState)) << std::endl;
      std::cout << "DEBUG onsuccess: status " << fetch->status << std::endl;
      std::cout << "DEBUG onsuccess: statusText " << fetch->statusText << std::endl;
      download_state = static_cast<readystate>(fetch->readyState);
      download_status = fetch->status;

      model_list_result = {};
      try {
        std::string_view const json_view{fetch->data, static_cast<size_t>(fetch->numBytes)};
        nlohmann::ordered_json const json = nlohmann::ordered_json::parse(json_view); // preserve element order when parsing
        for(auto const &model : json.at("data")) {
          model_list_result->emplace_back(model.at("id"));
        }
      } catch(std::exception const &e) {
        model_list_result = std::unexpected{std::string{"Failed to parse model list: "} + e.what()};
      }
      std::sort(model_list_result->begin(), model_list_result->end());
      model_selected = model_list_result->end();

      emscripten_fetch_close(fetch);                                            // free data associated with the fetch
    };
    attr.onerror = [](emscripten_fetch_t *fetch){
      // TODO: free requestData here
      download_state = static_cast<readystate>(fetch->readyState);
      download_status = fetch->status;
      model_list_result = std::unexpected{std::string{fetch->data, static_cast<size_t>(fetch->numBytes)}};
      emscripten_fetch_close(fetch);                                            // also free data on error
    };
    attr.onprogress = [](emscripten_fetch_t *fetch){
      // note: enable EMSCRIPTEN_FETCH_STREAM_DATA to populate fetch->data progressively
      std::cout << "DEBUG onprogress: readyState " << magic_enum::enum_name(static_cast<readystate>(fetch->readyState)) << std::endl;
      std::cout << "DEBUG onprogress: status " << fetch->status << std::endl;
      if(fetch->totalBytes == 0) {
        download_bytes_done = fetch->dataOffset + fetch->numBytes;
        download_bytes_total = {};
      } else {
        download_bytes_done = fetch->dataOffset;
        download_bytes_total = fetch->totalBytes;
      }

      download_state = static_cast<readystate>(fetch->readyState);
      download_status = fetch->status;
    };
    attr.onreadystatechange = [](emscripten_fetch_t *fetch){
      std::cout << "DEBUG onreadystatechange: readyState " << magic_enum::enum_name(static_cast<readystate>(fetch->readyState)) << std::endl;
      std::cout << "DEBUG onreadystatechange: status " << fetch->status << std::endl;
      download_state = static_cast<readystate>(fetch->readyState);
      download_status = fetch->status;
    };

    emscripten_fetch_t *fetch{emscripten_fetch(&attr, "https://api.openai.com/v1/models")};
    std::cout << "DEBUG result: id " << fetch->id << std::endl;
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
