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
#include <regex>
#include <iostream>

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

  struct PseudoEntry {
    bool fetched = false;
    FileKind kind;
    std::string name;
  };

  std::map<std::string, PseudoEntry> pseudo_entries;



  std::string dirPath;
  emscripten::ProxyWorker& proxy;

  const char* fileKindToString(FileKind kind) {
    switch (kind) {
      case DataFileKind: return "file";
      case DirectoryKind: return "directory";
      case SymlinkKind: return "symlink";
      default: return "unknown";
    }
  }

  // Function to process the fetch response and print file info
  void processFetchResponse(const std::string& url) {
    // Prepare fetch request attributes
    emscripten_fetch_attr_t fetchAttributes;
    emscripten_fetch_attr_init(&fetchAttributes);
    
    // Set flags for loading data to memory and synchronous fetch
    fetchAttributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

    // Perform the fetch request synchronously
    emscripten_fetch_t* fetchData = emscripten_fetch(&fetchAttributes, url.c_str());

    // Check if the fetch was successful
    if (fetchData->status == 404) {
        printf("fetchfs: createdir: fetch %s failed: 404", fetchData->url);
        return;
    }

    // Convert the fetched data into a string
    std::string responseText(fetchData->data, fetchData->numBytes);

    // Use a regular expression to find all href attributes in the HTML response
    std::regex linkRegex(R"(<a\s+href="([^"]+)\">([^<]+)</a>)");
    std::smatch match;

    // Search for links in the HTML response
    std::string::const_iterator searchStart(responseText.cbegin());
    while (std::regex_search(searchStart, responseText.cend(), match, linkRegex)) {
        // Extract href and the visible name
        std::string href = match[1];
        std::string linkName = match[2];

        // Assuming if the href ends with a "/" it's a directory, otherwise it's a file
        FileKind kind = href.back() == '/' ? DirectoryKind : DataFileKind;

        // Remove trailing slash if it exists in the name
        if (!linkName.empty() && linkName.back() == '/') {
            linkName.erase(linkName.size() - 1);
        }

        // Print the file or directory info
        std::cout << "kind: " << fileKindToString(kind) << " name: " << linkName << std::endl;

        // Create a PseudoEntry for the current link
        PseudoEntry entry;
        entry.name = linkName;
        entry.kind = kind;

        // Add the entry to the map
        pseudo_entries[linkName] = entry;

        // Move to the next match
        searchStart = match.suffix().first;

        if (kind == DataFileKind)
          insertDataFile(linkName, mode);

    }
  }


public:
  FetchDirectory(const std::string& path,
                 mode_t mode,
                 backend_t backend,
                 emscripten::ProxyWorker& proxy)
    : MemoryDirectory(mode, backend), dirPath(path), proxy(proxy) {

    //createUnfetchedEntries(dirPath);
    std::cout << "path: " << path << " - " "dirpath: " <<dirPath << "\n";
    processFetchResponse(dirPath);


  }

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

  bool isDirectory(std::string name) {
    return pseudo_entries[name].kind == DirectoryKind;
  }


  bool isFetched(std::string name) {
    return pseudo_entries[name].fetched;
  }

  bool exists(std::string name) {
    return pseudo_entries.find(name) != pseudo_entries.end();
  }

  std::shared_ptr<File> fetchChild(std::string name) {
    auto child = MemoryDirectory::getChild(name);
    size_t size = child->locked().getSize();
    pseudo_entries[name].fetched = true;
    printf("fetchbackend: search: %s, size: %zu\n", name.c_str(), size);

    return child;
  }

  std::shared_ptr<Directory> fetchInsertChildDirectory(std::string name, mode_t mode) {
    auto newChild = insertDirectory(name, mode);
    printf("fetchbackend: newdir fetch: %s\n", name.c_str());
    pseudo_entries[name].fetched = true;
    return newChild;
  }

  std::shared_ptr<File> getChild(const std::string& name) override {
    if (!exists(name))
      return nullptr;

    if (isFetched(name))
      return MemoryDirectory::getChild(name);

//    auto child = MemoryDirectory::getChild(name);
//    if (child != nullptr) return child;
//    printf("fetch_backend: new impl: %s\n", getChildPath(name).c_str());

    if (isDirectory(name)){
      return fetchInsertChildDirectory(name, mode);
    }

    /////

    auto newChild = fetchChild(name);

    return newChild;;
    
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
