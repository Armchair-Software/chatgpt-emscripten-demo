#include <iostream>
#include "gpt_interface.h"
#include <imgui/imgui.h>
#include <imgui/imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

using namespace std::string_literals;

namespace gui {

void gpt_interface::draw() {
  /// Draw the interface window
  auto &imgui_io{ImGui::GetIO()};
  if(!ImGui::Begin("Chat", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::End();
    return;
  }
  ImGui::SetWindowPos({0.0f, 0.0f}, ImGuiCond_FirstUseEver);
  ImGui::SetWindowSize(imgui_io.DisplaySize);

  ImGui::InputTextWithHint("API key", "Paste OpenAI API key here", &api_key, ImGuiInputTextFlags_Password);

  if(ImGui::Button("Request list of models")) {
    model_list_result = {};

    fetcher.fetch({
      .url{"https://api.openai.com/v1/models"},
      .headers{
        "Authorization", "Bearer " + api_key,
      },
      .on_success{[&](unsigned short status, std::string_view data){
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
      .on_error{[&](unsigned short status, std::string_view status_text, std::string_view data){
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
          nlohmann::json request_json = {
            {"model", "gpt-4o"},
            {"response_format", {
              {"type", "text"}
            }},
            {"temperature", 1},
            {"max_tokens", 2048},
            {"top_p", 1},
            {"frequency_penalty", 0},
            {"presence_penalty", 0},
          };
          for(auto const &message : messages) {
            request_json["messages"].emplace_back(
              nlohmann::json{
                {"role", magic_enum::enum_name(message.role)},
                {"content", {
                  {
                    {"type", "text"},
                    {"text", message.text}
                  }
                }}
              }
            );
          }

          fetcher.fetch({
            .method{"POST"},
            .url{"https://api.openai.com/v1/chat/completions"},
            .headers{
              "Content-Type", "application/json",
              "Authorization", "Bearer " + api_key,
            },
            .body{request_json.dump()},
            .on_success{[&](unsigned short status, std::string_view data){
              nlohmann::json const json = nlohmann::ordered_json::parse(data);
              messages.emplace_back(message_type{
                .role{message_type::roles::assistant},
                .text{json.at("choices").front().at("message").at("content")},
              });
              messages.emplace_back(message_type{
                .role{message_type::roles::user},
              });
            }},
            .on_error{[](unsigned short status, std::string_view status_text, std::string_view data){
              std::cerr << "ERROR calling API: " << status << ": " << status_text << ", " << data << std::endl;
              // TODO: error message box in gui
            }},
          });
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

/**
sample response:

{
  "id": "chatcmpl-AcZ8FxqQeZTY1oMIHmq39u5gJWl40",
  "object": "chat.completion",
  "created": 1733754875,
  "model": "gpt-4o-2024-08-06",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "four",
        "refusal": null
      },
      "logprobs": null,
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 23,
    "completion_tokens": 1,
    "total_tokens": 24,
    "prompt_tokens_details": {
      "cached_tokens": 0,
      "audio_tokens": 0
    },
    "completion_tokens_details": {
      "reasoning_tokens": 0,
      "audio_tokens": 0,
      "accepted_prediction_tokens": 0,
      "rejected_prediction_tokens": 0
    }
  },
  "system_fingerprint": "fp_9d50cd990b"
}
**/
