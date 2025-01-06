#include "emscripten_fetch_manager.h"

emscripten_fetch_manager::request::request(std::unique_ptr<std::string const> &&this_data,
                                           std::function<void(unsigned short status, std::span<std::byte const> data)> &&this_callback_success,
                                           std::function<void(unsigned short status, std::string_view status_text, std::span<std::byte const> data)> &&this_callback_error)
  : data(std::move(this_data)),
    callback_success(std::move(this_callback_success)),
    callback_error(std::move(this_callback_error)) {
}

emscripten_fetch_manager::request_id emscripten_fetch_manager::fetch(request_params &&params) {
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
    request.state = static_cast<request::ready_state>(fetch->readyState);
    request.status = fetch->status;

    request.callback_success(fetch->status, std::as_bytes(std::span{fetch->data, static_cast<size_t>(fetch->numBytes)}));

    manager.requests.erase(fetch->id);
    emscripten_fetch_close(fetch);                                              // free data associated with the fetch
  };
  attr.onerror = [](emscripten_fetch_t *fetch){
    /// Error callback
    auto &manager{*static_cast<emscripten_fetch_manager*>(fetch->userData)};
    auto &request{manager.requests.at(fetch->id)};

    request.state = static_cast<request::ready_state>(fetch->readyState);
    request.status = fetch->status;

    request.callback_error(fetch->status, fetch->statusText, std::as_bytes(std::span{fetch->data, static_cast<size_t>(fetch->numBytes)}));

    manager.requests.erase(fetch->id);
    emscripten_fetch_close(fetch);                                              // also free data on error
  };
  attr.onprogress = [](emscripten_fetch_t *fetch){
    /// Progress updated callback
    // note: enable EMSCRIPTEN_FETCH_STREAM_DATA to populate fetch->data progressively
    auto &manager{*static_cast<emscripten_fetch_manager*>(fetch->userData)};
    auto &request{manager.requests.at(fetch->id)};
    request.state = static_cast<request::ready_state>(fetch->readyState);
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
    request.state = static_cast<request::ready_state>(fetch->readyState);
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
