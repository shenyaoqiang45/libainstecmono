// record_playback.hpp - BackendDevice wrappers for .acam record / playback.
#pragma once

#include <memory>
#include <string>

#include "camsdk_backend.h"

namespace acam {

// Wrap a live device so each successful capture is appended to path.
// Fails if inner is not open or path cannot be created.
int begin_recording(std::shared_ptr<BackendDevice>& device,
                    const std::string& path,
                    std::string& err);

// Stop recording and unwrap to the inner live device. Idempotent if not
// currently a RecordingDevice.
int end_recording(std::shared_ptr<BackendDevice>& device, std::string& err);

// Load a playback BackendDevice from an .acam file (does not open the file
// until BackendDevice::open).
std::shared_ptr<BackendDevice> create_playback_device(const std::string& path,
                                                      std::string& err);

} // namespace acam
