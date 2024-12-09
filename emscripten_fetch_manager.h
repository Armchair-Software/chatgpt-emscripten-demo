#pragma once
#include <functional>
#include <string>
#include <vector>
#include <emscripten/fetch.h>

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
    enum class ready_state : unsigned short {                                   // from include/emscripten/fetch.h
      unsent,
      opened,
      headers_received,
      loading,
      done,
    } state{ready_state::opened};                                               // current state of the request - considered opened as soon as the request is created
    unsigned short status{0};                                                   // HTTP status of the request while in progress (200, 404, etc)
    uint64_t bytes_done{0};
    std::optional<uint64_t> bytes_total{};

    request(std::unique_ptr<std::string const> &&data,
            std::function<void(unsigned short status, std::string_view data)> &&callback_success,
            std::function<void(unsigned short status, std::string_view status_text, std::string_view data)> &&callback_error);
  };
  std::unordered_map<request_id, request> requests;

public:
  request_id fetch(request_params &&params);
};
