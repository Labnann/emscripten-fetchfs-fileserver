// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the JS file backend and JS file of the new file system.
// See https://github.com/emscripten-core/emscripten/issues/15041.

#include "fetch_backend.h"
#include "backend.h"
#include "proxied_async_js_impl_backend.h"
#include "wasmfs.h"
#include <emscripten/fetch.h>

namespace wasmfs {

class FetchFile : public ProxiedAsyncJSImplFile {
  std::string filePath;

public:
  FetchFile(const std::string& path,
            mode_t mode,
            backend_t backend,
            emscripten::ProxyWorker& proxy)
    : ProxiedAsyncJSImplFile(mode, backend, proxy), filePath(path) {}

  const std::string& getPath() const { return filePath; }
};

class FetchDirectory : public MemoryDirectory {
  std::string dirPath;
  emscripten::ProxyWorker& proxy;

public:
  FetchDirectory(const std::string& path,
                 mode_t mode,
                 backend_t backend,
                 emscripten::ProxyWorker& proxy)
    : MemoryDirectory(mode, backend), dirPath(path), proxy(proxy) {}

  std::shared_ptr<DataFile> insertDataFile(const std::string& name,
                                           mode_t mode) override {
    auto childPath = getChildPath(name);
    auto child =
      std::make_shared<FetchFile>(childPath, mode, getBackend(), proxy);
    insertChild(name, child);
    return child;
  }

  std::shared_ptr<Directory> insertDirectory(const std::string& name,
                                             mode_t mode) override {
    auto childPath = getChildPath(name);
    auto childDir =
      std::make_shared<FetchDirectory>(childPath, mode, getBackend(), proxy);
    insertChild(name, childDir);
    return childDir;
  }

  std::string getChildPath(const std::string& name) const {
    return dirPath + '/' + name;
  }


  int isDirectory(emscripten_fetch_t* fetch) {
    // Check if the server provided a 'Content-Type' header.
    size_t header_length = emscripten_fetch_get_response_headers_length(fetch);
    char* header_string = (char*) malloc(header_length + 1);
  
    emscripten_fetch_get_response_headers(fetch, header_string, header_length+1);

    char** header_array = emscripten_fetch_unpack_response_headers(header_string);


    for (int i = 0; header_array[i]; i+=2){
      printf("h:: %s : %s \n", header_array[i], header_array[i+1]);
      if(!strcmp(header_array[i], "content-type" )) {
        if (!strcmp(header_array[i+1], "text/html")) {
          printf("found a directory\n");
          return true;
        }
        printf("not a directory: %s \n", fetch->url);
        return false;
      }
    }

    printf("failed to find content-type header\n");
    return 0;

    /*
    //const char* contentType = fetch->headers["Content-Type"].c_str();
    if (contentType != nullptr) {
        // If the Content-Type is text/html, it's likely an HTML page (could be a directory listing).
        if (strstr(contentType, "text/html") != nullptr) {
            return true;  // It's a directory or an HTML page.
        }
    }*/ 
  }

  std::shared_ptr<File> getChild(const std::string& name) override {
    auto child = MemoryDirectory::getChild(name);
    if (child != nullptr) return child;
    printf("fetch_backend: new impl: %s\n", getChildPath(name).c_str());
    //////
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "HEAD");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
    attr.timeoutMSecs = 20;
    emscripten_fetch_t *fetch = emscripten_fetch(&attr, getChildPath(name).c_str()); // Blocks here until the operation is complete.
    if (fetch->status == 404) {
      printf("Downloading %s failed, HTTP failure status code: %d.\n", fetch->url, fetch->status);
      emscripten_fetch_close(fetch);
      return nullptr;
    }

    printf("Finished downloading %llu bytes from URL %s.\n", fetch->numBytes, fetch->url);

    if (isDirectory(fetch)){
      printf("fetch_new_backend: newdir: %s\n", name.c_str());
      auto newChild = insertDirectory(name, mode);
      return newChild;
    }
    emscripten_fetch_close(fetch);

    auto newChild = insertDataFile(name, mode);
    int fsize = newChild->locked().getSize();
    printf("fetchbackend: search: %s, size: %d\n", name.c_str(), fsize);

    if (fsize == 0) return nullptr;

    return newChild;;
    /////
  }
  
};

class FetchBackend : public ProxiedAsyncJSBackend {
  std::string baseUrl;

public:
  FetchBackend(const std::string& baseUrl,
               std::function<void(backend_t)> setupOnThread)
    : ProxiedAsyncJSBackend(setupOnThread), baseUrl(baseUrl) {}

  std::shared_ptr<DataFile> createFile(mode_t mode) override {
    return std::make_shared<FetchFile>(baseUrl, mode, this, proxy);
  }

  std::shared_ptr<Directory> createDirectory(mode_t mode) override {
    return std::make_shared<FetchDirectory>(baseUrl, mode, this, proxy);
  }
};

extern "C" {
backend_t wasmfs_create_fetch_backend(const char* base_url) {
  // ProxyWorker cannot safely be synchronously spawned from the main browser
  // thread. See comment in thread_utils.h for more details.
  assert(!emscripten_is_main_browser_thread() &&
         "Cannot safely create fetch backend on main browser thread");
  return wasmFS.addBackend(std::make_unique<FetchBackend>(
    base_url ? base_url : "",
    [](backend_t backend) { _wasmfs_create_fetch_backend_js(backend); }));
}

const char* EMSCRIPTEN_KEEPALIVE _wasmfs_fetch_get_file_path(void* ptr) {
  auto* file = reinterpret_cast<wasmfs::FetchFile*>(ptr);
  return file ? file->getPath().data() : nullptr;
}
}

} // namespace wasmfs
