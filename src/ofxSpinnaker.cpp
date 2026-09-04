#include "ofxSpinnaker.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "ofFileUtils.h"
#include "ofUtils.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

namespace {

std::string getStringNode(INodeMap& nodeMap, const std::string& nodeName) {
    try {
        CStringPtr node = nodeMap.GetNode(nodeName.c_str());
        if (IsReadable(node)) {
            return node->GetValue().c_str();
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnaker") << "Failed to read node '" << nodeName << "': " << e.what();
    }
    return {};
}

// Spinnaker::System::GetInstance() loads its GenTL producer (.cti) itself -
// this isn't an optional interop path, it's how the C++ API talks to the
// transport layer at all. Point it at the copy addon_config.mk's ADDON_DATA
// bundles into bin/data/flir-gentl/ rather than requiring it installed
// system-wide, unless the caller has already set the variable themselves.
//
// The existing value is only trusted if it actually resolves: a launcher
// that doesn't source the same shell rc files as a manual terminal run
// (an IDE's build/debug task, a login item, etc.) can still inherit a
// stale SPINNAKER_GENTL64_CTI from elsewhere in the process tree, and an
// unresolved path there would otherwise silently shadow this fallback and
// break System::GetInstance() with no clear reason why it works manually
// but not from the IDE.
void ensureGenTLEnvironmentConfigured() {
    if (const char* existing = std::getenv("SPINNAKER_GENTL64_CTI"); existing && *existing) {
        if (ofFile::doesFileExist(existing)) {
            return;
        }
        ofLogWarning("ofxSpinnaker") << "SPINNAKER_GENTL64_CTI is set to '" << existing
                                      << "' but that file doesn't exist - ignoring it and"
                                      << " falling back to the bundled GenTL producer instead.";
    }

    const std::string ctiPath = ofToDataPath("flir-gentl/Spinnaker_GenTL.cti", true);
    if (ofFile::doesFileExist(ctiPath)) {
        setenv("SPINNAKER_GENTL64_CTI", ctiPath.c_str(), 1);
    } else {
        ofLogWarning("ofxSpinnaker") << "Bundled GenTL producer not found at " << ctiPath
                                      << " - camera discovery will likely fail. Re-run"
                                      << " scripts/sync_spinnaker_sdk.sh and rebuild.";
    }
}

} // namespace

// --------------------------------------------------------------
// ofxSpinnaker
// --------------------------------------------------------------

ofxSpinnaker::ofxSpinnaker() = default;

ofxSpinnaker::~ofxSpinnaker() {
    shutdown();
}

bool ofxSpinnaker::setup(const std::string& configurationDirectoryPath) {
    if (!configurationDirectoryPath.empty()) {
        setConfigurationDirectory(configurationDirectoryPath);
    } else if (configurationDirectory.empty()) {
        setConfigurationDirectory(ofToDataPath("spinnaker_configs", true));
    }

    std::lock_guard<std::mutex> lock(cameraMutex);

    ensureGenTLEnvironmentConfigured();

    try {
        system = System::GetInstance();
        if (!system) {
            ofLogError("ofxSpinnaker") << "Failed to obtain Spinnaker system instance.";
            return false;
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnaker") << "Spinnaker system initialization failed: " << e.what();
        return false;
    }

    discoverCameras();

    if (cameras.empty()) {
        ofLogWarning("ofxSpinnaker") << "No FLIR/Teledyne cameras detected.";
    }

    return true;
}

void ofxSpinnaker::setConfigurationDirectory(const std::string& directory) {
    std::string resolved = directory;

    if (resolved.empty()) {
        return;
    }

    if (!ofFilePath::isAbsolute(resolved)) {
        resolved = ofToDataPath(resolved, true);
    }

    ofDirectory dir(resolved);
    if (!dir.exists()) {
        dir.create(true);
    }

    {
        std::lock_guard<std::mutex> lock(cameraMutex);
        configurationDirectory = resolved;

        for (auto& camera : cameras) {
            if (camera) {
                camera->setConfigurationDirectory(configurationDirectory);
            }
        }
    }
}

void ofxSpinnaker::shutdown() {
    std::lock_guard<std::mutex> lock(cameraMutex);

    for (auto& camera : cameras) {
        if (camera) {
            camera->shutdown();
        }
    }
    cameras.clear();

    if (system) {
        try {
            system->ReleaseInstance();
        } catch (const Spinnaker::Exception& e) {
            ofLogWarning("ofxSpinnaker") << "Failed to release Spinnaker system: " << e.what();
        }
        system = nullptr;
    }
}

void ofxSpinnaker::update() {
    std::vector<std::shared_ptr<ofxSpinnakerCamera>> localCameras;
    {
        std::lock_guard<std::mutex> lock(cameraMutex);
        localCameras = cameras;
    }

    for (auto& camera : localCameras) {
        if (camera) {
            camera->update();
        }
    }
}

void ofxSpinnaker::draw(int cameraIndex, float x, float y) const {
    draw(cameraIndex, x, y, 0.0f, 0.0f);
}

void ofxSpinnaker::draw(int cameraIndex, float x, float y, float width, float height) const {
    std::shared_ptr<ofxSpinnakerCamera> camera;
    {
        std::lock_guard<std::mutex> lock(cameraMutex);
        if (cameraIndex < 0 || static_cast<size_t>(cameraIndex) >= cameras.size()) {
            ofLogWarning("ofxSpinnaker") << "draw() called with invalid camera index " << cameraIndex;
            return;
        }
        camera = cameras.at(static_cast<size_t>(cameraIndex));
    }

    if (!camera) {
        return;
    }

    if (width <= 0.0f || height <= 0.0f) {
        camera->draw(x, y);
    } else {
        camera->draw(x, y, width, height);
    }
}

size_t ofxSpinnaker::getNumCameras() const {
    std::lock_guard<std::mutex> lock(cameraMutex);
    return cameras.size();
}

std::shared_ptr<ofxSpinnakerCamera> ofxSpinnaker::getCamera(size_t index) const {
    std::lock_guard<std::mutex> lock(cameraMutex);
    if (index >= cameras.size()) {
        return nullptr;
    }
    return cameras.at(index);
}

const ofParameterGroup& ofxSpinnaker::getCameraParameterGroup(size_t index) const {
    static ofParameterGroup emptyGroup;
    auto camera = getCamera(index);
    return camera ? camera->getParameterRoot() : emptyGroup;
}

void ofxSpinnaker::refreshCameraList() {
    std::lock_guard<std::mutex> lock(cameraMutex);
    discoverCameras();
}

void ofxSpinnaker::discoverCameras() {
    if (!system) {
        ofLogError("ofxSpinnaker") << "discoverCameras() called without a valid system instance.";
        return;
    }

    ofLogNotice("ofxSpinnaker") << "Scanning for cameras...";

    CameraList camList = system->GetCameras();
    const size_t numCameras = camList.GetSize();

    std::unordered_map<std::string, std::shared_ptr<ofxSpinnakerCamera>> existing;
    for (auto& camera : cameras) {
        if (camera) {
            existing[camera->getSerialNumber()] = camera;
        }
    }

    std::vector<std::shared_ptr<ofxSpinnakerCamera>> refreshed;
    refreshed.reserve(numCameras);

    for (size_t i = 0; i < numCameras; ++i) {
        CameraPtr cameraPtr = camList.GetByIndex(i);

        std::string serial = "unknown-" + ofToString(i);
        try {
            INodeMap& deviceNodeMap = cameraPtr->GetTLDeviceNodeMap();
            const std::string serialCandidate = getStringNode(deviceNodeMap, "DeviceSerialNumber");
            if (!serialCandidate.empty()) {
                serial = serialCandidate;
            }
        } catch (const Spinnaker::Exception& e) {
            ofLogWarning("ofxSpinnaker") << "Failed to query serial number for camera index " << i << ": " << e.what();
        }

        std::shared_ptr<ofxSpinnakerCamera> camera;

        auto existingIt = existing.find(serial);
        if (existingIt != existing.end()) {
            camera = existingIt->second;
            if (camera) {
                camera->setConfigurationDirectory(configurationDirectory);
            }
            existing.erase(existingIt);
        } else {
            camera = std::make_shared<ofxSpinnakerCamera>(cameraPtr);
            if (camera) {
                camera->setConfigurationDirectory(configurationDirectory);
            }
            if (!camera || !camera->setup()) {
                ofLogError("ofxSpinnaker") << "Failed to initialize camera with serial " << serial;
                continue;
            }
        }

        refreshed.emplace_back(camera);
    }

    camList.Clear();

    // Shutdown any cameras that disappeared
    for (auto& entry : existing) {
        if (entry.second) {
            ofLogNotice("ofxSpinnaker") << "Camera " << entry.first << " disconnected.";
            entry.second->shutdown();
        }
    }

    cameras = std::move(refreshed);

    ofLogNotice("ofxSpinnaker") << cameras.size() << " camera(s) ready.";
}

// --------------------------------------------------------------
// ofxSpinnakerCamera
// --------------------------------------------------------------

ofxSpinnakerCamera::ofxSpinnakerCamera(Spinnaker::CameraPtr cameraPtr)
: camera(std::move(cameraPtr)) {
    processor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_HQ_LINEAR);
    // Always exactly one entry so getTexturePlanes()/getTexture() have a
    // valid reference even before the first frame arrives.
    texturePlanes.resize(1);
}

ofxSpinnakerCamera::~ofxSpinnakerCamera() {
    shutdown();
}

bool ofxSpinnakerCamera::setup() {
    if (!camera || !camera->IsValid()) {
        ofLogError("ofxSpinnakerCamera") << "Invalid camera pointer during setup.";
        return false;
    }

    try {
        INodeMap& deviceNodeMap = camera->GetTLDeviceNodeMap();
        serialNumber = getStringNode(deviceNodeMap, "DeviceSerialNumber");
        deviceDisplayName = getStringNode(deviceNodeMap, "DeviceModelName");
        if (deviceDisplayName.empty()) {
            deviceDisplayName = getStringNode(deviceNodeMap, "DeviceDisplayName");
        }
        if (deviceDisplayName.empty()) {
            deviceDisplayName = serialNumber.empty() ? std::string("Spinnaker Camera") : serialNumber;
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to query device info prior to init: " << e.what();
    }

    try {
        camera->Init();
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Failed to initialize camera " << serialNumber << ": " << e.what();
        return false;
    }

    if (!configDirectory.empty()) {
        std::string fileStem = !serialNumber.empty() ? serialNumber : deviceDisplayName;
        if (fileStem.empty()) {
            fileStem = "camera";
        }
        std::string sanitized = fileStem;
        std::replace(sanitized.begin(), sanitized.end(), ' ', '_');
        configFilePath = ofFilePath::join(configDirectory, sanitized + ".json");
    }

    // Lowest-latency buffering for a live-display use case, per the SDK's own
    // guidance: always hand the app the newest frame rather than queuing stale
    // ones. Must happen before the first BeginAcquisition().
    configureStreamBuffering();

    buildParameterTree();
    attachParameterListener();
    loadConfiguration();
    parameterRevision.store(1, std::memory_order_relaxed);
    guiRefreshRequested.store(true, std::memory_order_release);

    try {
        camera->RegisterEventHandler(*this);
        imageEventHandlerRegistered = true;
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Failed to register image event handler on " << serialNumber << ": " << e.what();
        return false;
    }

    startStreaming();

    if (streaming && !controlsClassified) {
        // Now that the stream is actually running, discover which controls
        // really require it to be stopped versus which stay writable live.
        // This one-time probe replaces guessing from node names.
        classifyControlGroups();
        controlsClassified = true;

        // Rebuild once so any control reclassified as "stop capture required"
        // moves out of Live Controls into Reconfigure Controls before the GUI
        // ever draws a frame.
        skipConfigReload = true;
        rebuildParameters();
        skipConfigReload = false;
    }

    ofLogNotice("ofxSpinnakerCamera") << "Camera " << serialNumber << " initialized."
                                       << " Device: " << deviceDisplayName;

    initializedFlag = true;
    return true;
}

void ofxSpinnakerCamera::shutdown() {
    initializedFlag = false;
    stopStreaming();

    if (imageEventHandlerRegistered && camera && camera->IsValid()) {
        try {
            camera->UnregisterEventHandler(*this);
        } catch (const Spinnaker::Exception& e) {
            ofLogWarning("ofxSpinnakerCamera") << "Failed to unregister image event handler on " << serialNumber << ": " << e.what();
        }
        imageEventHandlerRegistered = false;
    }

    if (listenerAttached) {
        parameterListener.unsubscribe();
        listenerAttached = false;
    }

    if (camera && camera->IsValid()) {
        resetGevHeartbeatIfPresent();
        try {
            camera->DeInit();
        } catch (const Spinnaker::Exception& e) {
            ofLogWarning("ofxSpinnakerCamera") << "Failed to de-initialize camera " << serialNumber << ": " << e.what();
        }
        camera = nullptr;
    }
}

void ofxSpinnakerCamera::startStreaming() {
    if (!camera || !camera->IsValid()) {
        return;
    }
    if (streaming) {
        return;
    }

    try {
        camera->BeginAcquisition();
        streaming = true;
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Failed to begin acquisition on " << serialNumber << ": " << e.what();
    }
}

void ofxSpinnakerCamera::stopStreaming() {
    if (!camera || !camera->IsValid()) {
        streaming = false;
        return;
    }
    if (!streaming) {
        return;
    }

    streaming = false;

    try {
        camera->EndAcquisition();
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to end acquisition on " << serialNumber << ": " << e.what();
    }
}

bool ofxSpinnakerCamera::isStreaming() const {
    return streaming;
}

void ofxSpinnakerCamera::update() {
    if (pendingRebuildRequested) {
        // Deferred from onParameterChanged(): rebuilding the ofParameterGroup
        // tree here (main thread, between frames) rather than inline from the
        // parameter-changed callback avoids invalidating a GUI's live iteration
        // over the same group (e.g. an ImGui widget triggering a structural
        // rebuild mid-draw).
        pendingRebuildRequested = false;

        if (pendingRebuildSkipConfigReload) {
            // The value that triggered this rebuild was itself just restored
            // from the saved configuration (see requestDeferredRebuild()) -
            // reapplying the whole config again here would just repeat the
            // same writes (and any of their "not writable"/out-of-range
            // warnings) a second time for no benefit.
            pendingRebuildSkipConfigReload = false;
            skipConfigReload = true;
            rebuildParameters();
            skipConfigReload = false;
        } else {
            rebuildParameters();
        }
    }

    // isFrameNew() (ofBaseVideo contract) is true for exactly one update()
    // call following a newly-arrived frame, matching ofVideoGrabber's own
    // semantics - reset here, set below only if a frame was actually consumed.
    frameIsNew = false;

    {
        std::unique_lock<std::mutex> lock(frameMutex);
        if (newFrameAvailable) {
            if (useTexture) {
                ensureTextureMatches(pixels);
                texturePlanes[0].loadData(pixels);
            }
            newFrameAvailable = false;
            frameIsNew = true;
        }
    }

    if (configDirty.load(std::memory_order_acquire) && !loadingConfiguration) {
        const uint64_t now = ofGetElapsedTimeMillis();
        if (now - lastConfigSaveTime > 100) {
            saveConfiguration();
        }
    }

    updateMeasuredFps();
}

void ofxSpinnakerCamera::draw(float x, float y) const {
    if (texturePlanes[0].isAllocated()) {
        texturePlanes[0].draw(x, y);
    }
}

void ofxSpinnakerCamera::draw(float x, float y, float width, float height) const {
    if (texturePlanes[0].isAllocated()) {
        texturePlanes[0].draw(x, y, width, height);
    }
}

ofTexture& ofxSpinnakerCamera::getTexture() {
    return texturePlanes[0];
}

const ofTexture& ofxSpinnakerCamera::getTexture() const {
    return texturePlanes[0];
}

ofPixels& ofxSpinnakerCamera::getPixels() {
    return pixels;
}

const ofPixels& ofxSpinnakerCamera::getPixels() const {
    return pixels;
}

bool ofxSpinnakerCamera::isFrameNew() const {
    return frameIsNew;
}

void ofxSpinnakerCamera::close() {
    shutdown();
}

bool ofxSpinnakerCamera::isInitialized() const {
    return initializedFlag;
}

bool ofxSpinnakerCamera::setPixelFormat(ofPixelFormat pixelFormat) {
    if (pixelFormat == OF_PIXELS_RGB || pixelFormat == OF_PIXELS_GRAY) {
        outputPixelFormat.store(pixelFormat, std::memory_order_relaxed);
        return true;
    }
    ofLogWarning("ofxSpinnakerCamera") << "setPixelFormat: only OF_PIXELS_RGB and OF_PIXELS_GRAY are supported "
                                          "(the capture pipeline always converts through Spinnaker's ImageProcessor "
                                          "to one of those two); ignoring the requested format.";
    return false;
}

ofPixelFormat ofxSpinnakerCamera::getPixelFormat() const {
    return outputPixelFormat.load(std::memory_order_relaxed);
}

std::vector<ofVideoDevice> ofxSpinnakerCamera::listDevices() const {
    // This object represents one already-discovered camera, not a device
    // enumerator - real multi-camera discovery is ofxSpinnaker's job
    // (getNumCameras()/getCamera()). This just self-describes for code
    // written against the generic ofBaseVideoGrabber interface.
    ofVideoDevice device;
    device.id = 0;
    device.deviceName = deviceDisplayName;
    device.hardwareName = deviceDisplayName;
    device.serialID = serialNumber;
    device.bAvailable = (camera != nullptr);
    return {device};
}

bool ofxSpinnakerCamera::setup(int w, int h) {
    (void)w;
    (void)h;
    if (isInitialized()) {
        return true;
    }
    ofLogWarning("ofxSpinnakerCamera") << "setup(int,int) called before this camera was discovered/initialized by "
                                          "ofxSpinnaker - real initialization happens automatically on discovery "
                                          "(ofxSpinnaker::setup()/refreshCameraList()), not through this "
                                          "compatibility entry point, so there's no hardware to set up here yet.";
    return false;
}

float ofxSpinnakerCamera::getWidth() const {
    return static_cast<float>(pixels.getWidth());
}

float ofxSpinnakerCamera::getHeight() const {
    return static_cast<float>(pixels.getHeight());
}

void ofxSpinnakerCamera::setUseTexture(bool useTex) {
    useTexture = useTex;
}

bool ofxSpinnakerCamera::isUsingTexture() const {
    return useTexture;
}

std::vector<ofTexture>& ofxSpinnakerCamera::getTexturePlanes() {
    return texturePlanes;
}

const std::vector<ofTexture>& ofxSpinnakerCamera::getTexturePlanes() const {
    return texturePlanes;
}

const std::string& ofxSpinnakerCamera::getSerialNumber() const {
    return serialNumber;
}

const std::string& ofxSpinnakerCamera::getDeviceDisplayName() const {
    return deviceDisplayName;
}

ofParameterGroup& ofxSpinnakerCamera::getParameterRoot() {
    return rootParameters;
}

const std::unordered_map<std::string, std::vector<std::string>>& ofxSpinnakerCamera::getEnumDisplayNames() const {
    return enumDisplayNames;
}

uint64_t ofxSpinnakerCamera::getParameterRevision() const {
    return parameterRevision.load(std::memory_order_acquire);
}

bool ofxSpinnakerCamera::consumeGuiRefreshFlag() {
    return guiRefreshRequested.exchange(false, std::memory_order_acq_rel);
}

bool ofxSpinnakerCamera::isParameterCurrentlyWritable(const ofAbstractParameter& parameter) const {
    const ofAbstractParameter* parameterPtr = &parameter;
    auto pathIt = parameterPaths.find(parameterPtr);
    if (pathIt == parameterPaths.end()) {
        return false;
    }

    auto nodeIt = writableNodes.find(pathIt->second);
    if (nodeIt == writableNodes.end() || !nodeIt->second) {
        return false;
    }

    try {
        return IsWritable(nodeIt->second) && IsAvailable(nodeIt->second);
    } catch (const Spinnaker::Exception&) {
        return false;
    }
}

const std::vector<std::string>* ofxSpinnakerCamera::getEnumEntryNames(const ofAbstractParameter& parameter) const {
    auto pathIt = parameterPaths.find(&parameter);
    if (pathIt == parameterPaths.end()) {
        return nullptr;
    }

    auto namesIt = enumDisplayNames.find(pathIt->second);
    if (namesIt == enumDisplayNames.end()) {
        return nullptr;
    }

    return &namesIt->second;
}

void ofxSpinnakerCamera::setConfigurationDirectory(const std::string& directory) {
    if (directory.empty()) {
        return;
    }

    std::string resolved = directory;
    if (!ofFilePath::isAbsolute(resolved)) {
        resolved = ofToDataPath(resolved, true);
    }

    ofDirectory dir(resolved);
    if (!dir.exists()) {
        dir.create(true);
    }

    configDirectory = resolved;
    configLoaded = false;
    cachedConfiguration.clear();
    configDirty.store(false, std::memory_order_release);

    if (!serialNumber.empty()) {
        std::string fileStem = serialNumber;
        if (fileStem.empty()) {
            fileStem = "camera";
        }
        std::string sanitized = fileStem;
        std::replace(sanitized.begin(), sanitized.end(), ' ', '_');
        configFilePath = ofFilePath::join(configDirectory, sanitized + ".json");
    }
}

void ofxSpinnakerCamera::loadConfiguration() {
    if (configDirectory.empty()) {
        cachedConfiguration = captureConfigurationSnapshot();
        configLoaded = true;
        configDirty.store(true, std::memory_order_release);
        return;
    }

    if (configFilePath.empty()) {
        if (serialNumber.empty() && deviceDisplayName.empty()) {
            return;
        }
        std::string fileStem = !serialNumber.empty() ? serialNumber : deviceDisplayName;
        if (fileStem.empty()) {
            fileStem = "camera";
        }
        std::string sanitized = fileStem;
        std::replace(sanitized.begin(), sanitized.end(), ' ', '_');
        configFilePath = ofFilePath::join(configDirectory, sanitized + ".json");
    }

    ofDirectory dir(configDirectory);
    if (!dir.exists()) {
        dir.create(true);
    }

    ofFile configFile(configFilePath, ofFile::Reference);
    if (!configFile.exists()) {
        cachedConfiguration = captureConfigurationSnapshot();
        configLoaded = true;
        configDirty.store(true, std::memory_order_release);
        return;
    }

    try {
        std::ifstream in(configFilePath);
        if (!in.good()) {
            throw std::runtime_error("unable to open config file");
        }

        ofJson json;
        in >> json;
        in.close();

        cachedConfiguration = json;

        if (json.contains("parameters") && json["parameters"].is_object()) {
            loadingConfiguration = true;
            applyAllParametersFromJson(json["parameters"]);
            loadingConfiguration = false;
        }

        cachedConfiguration = captureConfigurationSnapshot();
        configLoaded = true;
        configDirty.store(true, std::memory_order_release);
        guiRefreshRequested.store(true, std::memory_order_release);
    } catch (const std::exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to load configuration for " << serialNumber << ": " << e.what();
        cachedConfiguration = captureConfigurationSnapshot();
        configLoaded = true;
        configDirty.store(true, std::memory_order_release);
    }
}

void ofxSpinnakerCamera::saveConfiguration() {
    if (configDirectory.empty()) {
        return;
    }

    if (cachedConfiguration.is_null()) {
        cachedConfiguration = captureConfigurationSnapshot();
    } else {
        cachedConfiguration["device"]["serialNumber"] = serialNumber;
        cachedConfiguration["device"]["model"] = deviceDisplayName;
        cachedConfiguration["device"]["timestamp"] = ofGetTimestampString();
        cachedConfiguration["parameters"] = ofJson::object();
        collectAllParameters(cachedConfiguration["parameters"]);
    }

    if (configFilePath.empty()) {
        std::string fileStem = !serialNumber.empty() ? serialNumber : deviceDisplayName;
        if (fileStem.empty()) {
            fileStem = "camera";
        }
        std::string sanitized = fileStem;
        std::replace(sanitized.begin(), sanitized.end(), ' ', '_');
        configFilePath = ofFilePath::join(configDirectory, sanitized + ".json");
    }

    try {
        ofDirectory dir(configDirectory);
        if (!dir.exists()) {
            dir.create(true);
        }

        std::ofstream out(configFilePath, std::ios::out | std::ios::trunc);
        out << cachedConfiguration.dump(4);
        out.close();

        configDirty.store(false, std::memory_order_release);
        lastConfigSaveTime = ofGetElapsedTimeMillis();
    } catch (const std::exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to save configuration for " << serialNumber << ": " << e.what();
    }
}

ofJson ofxSpinnakerCamera::captureConfigurationSnapshot() const {
    ofJson json;
    json["device"]["serialNumber"] = serialNumber;
    json["device"]["model"] = deviceDisplayName;
    json["device"]["timestamp"] = ofGetTimestampString();

    ofJson params = ofJson::object();
    collectAllParameters(params);
    json["parameters"] = params;

    return json;
}

void ofxSpinnakerCamera::collectParameters(const ofParameterGroup& group,
                                           const std::string& prefix,
                                           ofJson& collection) const {
    for (auto& paramPtr : group) {
        auto parameter = paramPtr;
        const std::string name = parameter->getName();
        const std::string path = prefix.empty() ? name : prefix + "/" + name;

        if (parameter->type() == typeid(ofParameterGroup).name()) {
            collectParameters(parameter->castGroup(), path, collection);
        } else if (parameter->type() == typeid(ofParameter<int>).name()) {
            auto enumIt = enumDisplayNames.find(path);
            const ofParameter<int>& intParam = parameter->cast<int>();
            if (enumIt != enumDisplayNames.end()) {
                const auto& names = enumIt->second;
                int index = intParam.get();
                if (index >= 0 && index < static_cast<int>(names.size())) {
                    collection[path] = names[index];
                } else {
                    collection[path] = index;
                }
            } else {
                collection[path] = intParam.get();
            }
        } else if (parameter->type() == typeid(ofParameter<float>).name()) {
            collection[path] = parameter->cast<float>().get();
        } else if (parameter->type() == typeid(ofParameter<bool>).name()) {
            collection[path] = parameter->cast<bool>().get();
        } else if (parameter->type() == typeid(ofParameter<std::string>).name()) {
            collection[path] = parameter->cast<std::string>().get();
        }
    }
}

void ofxSpinnakerCamera::applyConfigurationToGroup(ofParameterGroup& group,
                                                    const std::string& prefix,
                                                    const ofJson& values) {
    for (auto& paramPtr : group) {
        auto parameter = paramPtr;
        const std::string name = parameter->getName();
        const std::string path = prefix.empty() ? name : prefix + "/" + name;

        if (parameter->type() == typeid(ofParameterGroup).name()) {
            applyConfigurationToGroup(parameter->castGroup(), path, values);
            continue;
        }

        auto valueIt = values.find(path);
        if (valueIt == values.end()) {
            continue;
        }

        const ofJson& value = *valueIt;

        if (parameter->type() == typeid(ofParameter<int>).name()) {
            ofParameter<int>& intParam = parameter->cast<int>();
            auto enumIt = enumDisplayNames.find(path);
            int targetIndex = intParam.get();

            if (enumIt != enumDisplayNames.end()) {
                const auto& names = enumIt->second;
                if (value.is_string()) {
                    const std::string target = value.get<std::string>();
                    auto strIt = std::find(names.begin(), names.end(), target);
                    if (strIt != names.end()) {
                        targetIndex = static_cast<int>(std::distance(names.begin(), strIt));
                    }
                } else if (value.is_number_integer()) {
                    targetIndex = value.get<int>();
                }
                targetIndex = ofClamp(targetIndex, intParam.getMin(), intParam.getMax());
                intParam.set(targetIndex);
            } else if (value.is_number()) {
                targetIndex = value.get<int>();
                targetIndex = ofClamp(targetIndex, intParam.getMin(), intParam.getMax());
                intParam.set(targetIndex);
            }
        } else if (parameter->type() == typeid(ofParameter<float>).name()) {
            if (value.is_number()) {
                ofParameter<float>& floatParam = parameter->cast<float>();
                float targetValue = static_cast<float>(value.get<double>());
                targetValue = ofClamp(targetValue, floatParam.getMin(), floatParam.getMax());
                floatParam.set(targetValue);
            }
        } else if (parameter->type() == typeid(ofParameter<bool>).name()) {
            ofParameter<bool>& boolParam = parameter->cast<bool>();
            if (value.is_boolean()) {
                boolParam.set(value.get<bool>());
            } else if (value.is_number()) {
                boolParam.set(value.get<double>() != 0.0);
            }
        } else if (parameter->type() == typeid(ofParameter<std::string>).name()) {
            if (value.is_string()) {
                parameter->cast<std::string>().set(value.get<std::string>());
            }
        }
    }
}

void ofxSpinnakerCamera::markConfigurationDirty() {
    cachedConfiguration = captureConfigurationSnapshot();
    configDirty.store(true, std::memory_order_release);
}

void ofxSpinnakerCamera::collectAllParameters(ofJson& collection) const {
    collectParameters(liveParameters, "", collection);
    collectParameters(reconfigureParameters, "", collection);
}

void ofxSpinnakerCamera::applyAllParametersFromJson(const ofJson& values) {
    // Restoring a saved configuration can touch several controls that each
    // individually require the stream to be stopped (PixelFormat, Width,
    // Height, binning, ...). Applying them one at a time would stop and
    // restart acquisition once per field; stop once for the whole batch
    // instead. Each individual applyXxx()'s stopAcquisitionForCriticalChange()
    // call checks isStreaming() itself, so once we've stopped it here those
    // calls just apply directly rather than each trying to restart in turn.
    const bool wasStreaming = isStreaming();
    if (wasStreaming) {
        stopStreaming();
    }

    applyConfigurationToGroup(liveParameters, "", values);
    applyConfigurationToGroup(reconfigureParameters, "", values);

    if (wasStreaming) {
        startStreaming();
    }
}

void ofxSpinnakerCamera::configureStreamBuffering() {
    if (!camera || !camera->IsValid()) {
        return;
    }

    try {
        INodeMap& streamNodeMap = camera->GetTLStreamNodeMap();

        CEnumerationPtr handlingMode = streamNodeMap.GetNode("StreamBufferHandlingMode");
        if (IsReadable(handlingMode) && IsWritable(handlingMode)) {
            CEnumEntryPtr newestOnly = handlingMode->GetEntryByName("NewestOnly");
            if (IsReadable(newestOnly)) {
                handlingMode->SetIntValue(newestOnly->GetValue());
            }
        }

        CEnumerationPtr countMode = streamNodeMap.GetNode("StreamBufferCountMode");
        if (IsReadable(countMode) && IsWritable(countMode)) {
            CEnumEntryPtr manual = countMode->GetEntryByName("Manual");
            if (IsReadable(manual)) {
                countMode->SetIntValue(manual->GetValue());
            }
        }

        CIntegerPtr bufferCount = streamNodeMap.GetNode("StreamBufferCountManual");
        if (IsReadable(bufferCount) && IsWritable(bufferCount)) {
            int64_t desired = 3;
            desired = std::max<int64_t>(bufferCount->GetMin(), std::min<int64_t>(desired, bufferCount->GetMax()));
            bufferCount->SetValue(desired);
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to configure stream buffering for " << serialNumber << ": " << e.what();
    }
}

void ofxSpinnakerCamera::resetGevHeartbeatIfPresent() {
    if (!camera || !camera->IsValid()) {
        return;
    }

    try {
        INodeMap& nodeMap = camera->GetNodeMap();
        CBooleanPtr heartbeatDisable = nodeMap.GetNode("GevGVCPHeartbeatDisable");
        if (IsReadable(heartbeatDisable) && IsWritable(heartbeatDisable) && heartbeatDisable->GetValue()) {
            heartbeatDisable->SetValue(false);
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to reset GVCP heartbeat for " << serialNumber << ": " << e.what();
    }
}

void ofxSpinnakerCamera::classifyControlGroups() {
    for (auto& entry : preStreamWritableSnapshot) {
        const std::string& path = entry.first;
        const bool wasWritable = entry.second;

        auto nodeIt = writableNodes.find(path);
        if (nodeIt == writableNodes.end() || !nodeIt->second) {
            continue;
        }

        bool nowWritable = false;
        try {
            nowWritable = IsWritable(nodeIt->second);
        } catch (const Spinnaker::Exception&) {
            nowWritable = false;
        }

        if (wasWritable && !nowWritable) {
            nodeClassification[path] = ofxSpinnakerControlClass::StopCaptureRequired;
        } else {
            nodeClassification[path] = ofxSpinnakerControlClass::RealTime;
        }
    }
}

void ofxSpinnakerCamera::OnImageEvent(Spinnaker::ImagePtr image) {
    if (!image || image->IsIncomplete()) {
        if (image && image->IsIncomplete()) {
            // This callback runs on Spinnaker's own acquisition thread, once
            // per frame. Under sustained packet loss (a network issue, not a
            // code bug - see GevSCPD/adapter/cable troubleshooting) this can
            // fire at full frame rate; logging every single one adds real
            // console-I/O overhead right on the acquisition hot path and
            // makes the whole app feel sluggish on top of the dropped frames
            // themselves. Throttle to at most once per second.
            ++incompleteImageCount;
            const uint64_t now = ofGetElapsedTimeMillis();
            if (now - lastIncompleteImageLogTime > 1000) {
                ofLogWarning("ofxSpinnakerCamera") << "Incomplete image on " << serialNumber
                                                   << " - status " << image->GetImageStatus()
                                                   << " (" << incompleteImageCount << " incomplete since last log)";
                lastIncompleteImageLogTime = now;
                incompleteImageCount = 0;
            }
        }
        return;
    }

    try {
        // setPixelFormat() only supports switching between the RGB8 default
        // and grayscale (see its comment); anything else falls back to RGB8.
        const bool grayscale = (outputPixelFormat.load(std::memory_order_relaxed) == OF_PIXELS_GRAY);
        ImagePtr converted = processor.Convert(image, grayscale ? PixelFormat_Mono8 : PixelFormat_RGB8);

        std::unique_lock<std::mutex> lock(frameMutex);
        pixels.setFromPixels(static_cast<unsigned char*>(converted->GetData()),
                             converted->GetWidth(),
                             converted->GetHeight(),
                             grayscale ? OF_IMAGE_GRAYSCALE : OF_IMAGE_COLOR);
        newFrameAvailable = true;
        ++frameCount;
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Image conversion error on " << serialNumber << ": " << e.what();
    }
}

void ofxSpinnakerCamera::buildParameterTree() {
    if (!camera || !camera->IsValid()) {
        return;
    }

    rootParameters.clear();
    infoParameters.clear();
    liveParameters.clear();
    reconfigureParameters.clear();
    naturalParameters.clear();
    advancedParameters.clear();
    writableNodes.clear();
    enumDisplayNames.clear();
    enumValueMaps.clear();
    parameterPaths.clear();
    selectorParameterPaths.clear();
    genicamNameToPath.clear();
    pathToParameter.clear();

    rootParameters.setName(deviceDisplayName.empty() ? "Camera" : deviceDisplayName);
    infoParameters.setName("Info");
    liveParameters.setName("Live Controls");
    reconfigureParameters.setName("Reconfigure Controls");
    naturalParameters.setName("Camera Controls");
    advancedParameters.setName("Advanced Controls");

    // Camera Controls/Advanced Controls first - they're the primary surface;
    // Info/Live/Reconfigure are the raw fallback ("Full Settings" in the
    // example GUI), built the same as before and left untouched.
    rootParameters.add(naturalParameters);
    rootParameters.add(advancedParameters);
    rootParameters.add(infoParameters);
    rootParameters.add(liveParameters);
    rootParameters.add(reconfigureParameters);

    try {
        buildInfoGroup(camera->GetTLDeviceNodeMap());
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to populate info group for " << serialNumber << ": " << e.what();
    }

    try {
        INodeMap& nodeMap = camera->GetNodeMap();
        CNodePtr rootNode = nodeMap.GetNode("Root");
        if (!rootNode || !IsReadable(rootNode)) {
            ofLogWarning("ofxSpinnakerCamera") << "Root node not readable for camera " << serialNumber;
            return;
        }

        FeatureList_t features;
        CCategoryPtr rootCategory = static_cast<CCategoryPtr>(rootNode);
        rootCategory->GetFeatures(features);

        for (auto& feature : features) {
            if (feature) {
                traverseNode(feature, liveParameters, reconfigureParameters, "");
            }
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Failed to traverse node map: " << e.what();
    }

    buildNaturalControls();
    buildAdvancedControls();
}

void ofxSpinnakerCamera::buildInfoGroup(INodeMap& nodeMap) {
    infoParameters.clear();
    infoParameters.setName("Info");

    const std::vector<std::pair<std::string, std::string>> infoNodes = {
        {"Vendor", getStringNode(nodeMap, "DeviceVendorName")},
        {"Model", getStringNode(nodeMap, "DeviceModelName")},
        {"Family", getStringNode(nodeMap, "DeviceFamilyName")},
        {"Serial", getStringNode(nodeMap, "DeviceSerialNumber")},
        {"Firmware Version", getStringNode(nodeMap, "DeviceFirmwareVersion")},
        {"Firmware Build Time", getStringNode(nodeMap, "DeviceFirmwareBuildTime")}
    };

    for (const auto& entry : infoNodes) {
        if (!entry.second.empty()) {
            ofParameter<std::string> param;
            param.set(entry.first, entry.second);
            infoParameters.add(param);
        }
    }
}

ofAbstractParameter* ofxSpinnakerCamera::findByGenICamName(const std::string& genicamName) const {
    auto nameIt = genicamNameToPath.find(genicamName);
    if (nameIt == genicamNameToPath.end()) {
        return nullptr;
    }
    auto paramIt = pathToParameter.find(nameIt->second);
    if (paramIt == pathToParameter.end()) {
        return nullptr;
    }
    return paramIt->second;
}

void ofxSpinnakerCamera::addAliasToGroup(ofParameterGroup& target, std::initializer_list<std::string> candidateNames) {
    for (const auto& genicamName : candidateNames) {
        ofAbstractParameter* param = findByGenICamName(genicamName);
        if (!param) {
            continue;
        }

        auto pathIt = parameterPaths.find(param);
        if (pathIt == parameterPaths.end()) {
            return;
        }
        const std::string path = pathIt->second;
        const std::string displayName = param->getName();

        target.add(*param);

        if (param->type() == typeid(ofParameter<int>).name()) {
            ofParameter<int>& stored = target.get<int>(displayName);
            parameterPaths[&stored] = path;
        } else if (param->type() == typeid(ofParameter<float>).name()) {
            ofParameter<float>& stored = target.get<float>(displayName);
            parameterPaths[&stored] = path;
        } else if (param->type() == typeid(ofParameter<bool>).name()) {
            ofParameter<bool>& stored = target.get<bool>(displayName);
            parameterPaths[&stored] = path;
        } else if (param->type() == typeid(ofParameter<std::string>).name()) {
            ofParameter<std::string>& stored = target.get<std::string>(displayName);
            parameterPaths[&stored] = path;
        }
        return;
    }
}

void ofxSpinnakerCamera::buildNaturalControls() {
    naturalParameters.clear();
    naturalParameters.setName("Camera Controls");

    // Exposure, gain, white balance, and frame rate are standard GenICam SFNC
    // features present on essentially every Spinnaker camera. Saturation and
    // Hue are Point-Grey/Blackfly-lineage ISP extensions that aren't part of
    // the documented GenICam surface at all (confirmed absent from the SDK's
    // CameraDefs.h) - present on some cameras, absent on others, and their
    // exact node names are a best guess from SFNC naming convention rather
    // than a documented identifier, hence multiple candidates for those.
    // Whatever findByGenICamName() can't find is simply skipped.
    addAliasToGroup(naturalParameters, {"ExposureAuto"});
    addAliasToGroup(naturalParameters, {"ExposureTime"});
    addAliasToGroup(naturalParameters, {"GainAuto"});
    addAliasToGroup(naturalParameters, {"Gain"});
    addAliasToGroup(naturalParameters, {"BalanceWhiteAuto"});
    addAliasToGroup(naturalParameters, {"BalanceRatioSelector"});
    addAliasToGroup(naturalParameters, {"BalanceRatio"});
    addAliasToGroup(naturalParameters, {"SaturationEnable", "SaturationEnabled"});
    addAliasToGroup(naturalParameters, {"SaturationAuto"});
    addAliasToGroup(naturalParameters, {"Saturation"});
    addAliasToGroup(naturalParameters, {"HueEnable", "HueEnabled"});
    addAliasToGroup(naturalParameters, {"Hue"});
    addAliasToGroup(naturalParameters, {"AcquisitionFrameRateEnable"});
    addAliasToGroup(naturalParameters, {"AcquisitionFrameRate"});
    addAliasToGroup(naturalParameters, {"AcquisitionResultingFrameRate"});

    // Measured, not requested: counts frames actually delivered through
    // OnImageEvent rather than reading a target/theoretical rate, so it
    // reflects reality even under packet loss (see AcquisitionResultingFrameRate
    // above, which is the camera's own theoretical figure for comparison).
    // Never registered in parameterPaths, so it always renders as read-only.
    actualFpsParameter.set("Actual FPS", ofToString(measuredFps, 1) + " fps");
    naturalParameters.add(actualFpsParameter);
}

void ofxSpinnakerCamera::buildFilteredPixelFormatControl() {
    // Restrict to entries both offered by the camera and known to convert
    // cleanly via Spinnaker::ImageProcessor::Convert() (our capture pipeline
    // always converts every frame to RGB8) - of the SDK's ~253 PixelFormat
    // entries, roughly 160+ are 3D/depth, confidence-map, compressed, or
    // planar/signed/float variants ImageProcessor doesn't accept as input.
    static const std::unordered_set<std::string> safePixelFormats = {
        "Mono8", "Mono16",
        "BayerRG8", "BayerGR8", "BayerGB8", "BayerBG8",
        "BayerRG16", "BayerGR16", "BayerGB16", "BayerBG16",
        "RGB8", "BGR8", "RGBa8", "BGRa8"
    };

    auto pathIt = genicamNameToPath.find("PixelFormat");
    if (pathIt == genicamNameToPath.end()) {
        return;
    }
    const std::string rawPath = pathIt->second;

    auto namesIt = enumDisplayNames.find(rawPath);
    auto valuesIt = enumValueMaps.find(rawPath);
    auto nodeIt = writableNodes.find(rawPath);
    if (namesIt == enumDisplayNames.end() || valuesIt == enumValueMaps.end() || nodeIt == writableNodes.end()) {
        return;
    }

    const std::vector<std::string>& allNames = namesIt->second;
    const std::map<int, int64_t>& allValues = valuesIt->second;

    // Recover the camera's current pixel format symbolic name so the
    // filtered index preserves the current selection rather than resetting
    // to entry 0 every rebuild.
    std::string currentSymbolic;
    auto currentParamIt = pathToParameter.find(rawPath);
    if (currentParamIt != pathToParameter.end() &&
        currentParamIt->second->type() == typeid(ofParameter<int>).name()) {
        const int currentRawIndex = currentParamIt->second->cast<int>().get();
        if (currentRawIndex >= 0 && currentRawIndex < static_cast<int>(allNames.size())) {
            currentSymbolic = allNames[currentRawIndex];
        }
    }

    std::vector<std::string> filteredNames;
    std::map<int, int64_t> filteredValues;
    int currentFilteredIndex = 0;

    for (size_t i = 0; i < allNames.size(); ++i) {
        if (safePixelFormats.count(allNames[i]) == 0) {
            continue;
        }
        auto valueIt = allValues.find(static_cast<int>(i));
        if (valueIt == allValues.end()) {
            continue;
        }
        const int filteredIndex = static_cast<int>(filteredNames.size());
        filteredValues[filteredIndex] = valueIt->second;
        if (allNames[i] == currentSymbolic) {
            currentFilteredIndex = filteredIndex;
        }
        filteredNames.push_back(allNames[i]);
    }

    if (filteredNames.empty()) {
        return;
    }

    // A separate synthetic path, not the raw PixelFormat one: this control's
    // indices are remapped against the filtered list, not the camera's full
    // enum, so it needs its own enumDisplayNames/enumValueMaps entry sharing
    // only the underlying NodePtr (and therefore the real value) with the
    // raw control in Full Settings.
    const std::string syntheticPath = "Advanced/Pixel Format (Simple)";
    enumDisplayNames[syntheticPath] = filteredNames;
    enumValueMaps[syntheticPath] = filteredValues;
    writableNodes[syntheticPath] = nodeIt->second;
    // PixelFormat always requires the stream to be stopped to change -
    // already confirmed empirically for the raw control by
    // classifyControlGroups(). This synthetic control shares the same
    // underlying node and therefore the same real constraint; there's no
    // separate pre/post-stream probe to derive it from for a path that only
    // exists here, so it's asserted directly instead.
    nodeClassification[syntheticPath] = ofxSpinnakerControlClass::StopCaptureRequired;

    ofParameter<int> parameter;
    parameter.set("Pixel Format", currentFilteredIndex, 0, static_cast<int>(filteredNames.size() - 1));
    advancedParameters.add(parameter);
    ofParameter<int>& stored = advancedParameters.get<int>("Pixel Format");
    parameterPaths[&stored] = syntheticPath;
    pathToParameter[syntheticPath] = &stored;
}

void ofxSpinnakerCamera::buildAdvancedControls() {
    advancedParameters.clear();
    advancedParameters.setName("Advanced Controls");

    // Resolution is deliberately limited to binning rather than free-form
    // Width/Height/Offset sliders: binning is always camera-validated (its
    // own min/max/increment), so there's no way to leave the camera in an
    // invalid ROI state through this tier. Free-form ROI stays available in
    // Full Settings for anyone who needs it.
    addAliasToGroup(advancedParameters, {"BinningHorizontal"});
    addAliasToGroup(advancedParameters, {"BinningVertical"});

    buildFilteredPixelFormatControl();
}

void ofxSpinnakerCamera::updateMeasuredFps() {
    const uint64_t now = ofGetElapsedTimeMillis();
    if (fpsWindowStartMs == 0) {
        fpsWindowStartMs = now;
        return;
    }

    const uint64_t elapsedMs = now - fpsWindowStartMs;
    if (elapsedMs < 1000) {
        return;
    }

    uint64_t framesInWindow = 0;
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        framesInWindow = frameCount;
        frameCount = 0;
    }

    measuredFps = static_cast<float>(framesInWindow) / (static_cast<float>(elapsedMs) / 1000.0f);
    fpsWindowStartMs = now;

    actualFpsParameter.set(ofToString(measuredFps, 1) + " fps");
}

void ofxSpinnakerCamera::traverseNode(NodePtr node,
                                      ofParameterGroup& liveContainer,
                                      ofParameterGroup& reconfigureContainer,
                                      const std::string& currentPath) {
    if (!node || !IsReadable(node) || !IsAvailable(node)) {
        return;
    }

    if (node->GetVisibility() == Spinnaker::GenApi::Invisible) {
        return;
    }

    const EInterfaceType interfaceType = node->GetPrincipalInterfaceType();

    if (interfaceType == intfICategory) {
        CCategoryPtr category = static_cast<CCategoryPtr>(node);
        if (!category) {
            return;
        }

        FeatureList_t features;
        category->GetFeatures(features);

        ofParameterGroup liveSubgroup;
        ofParameterGroup reconfigureSubgroup;
        const std::string name = category->GetDisplayName().c_str();
        liveSubgroup.setName(name);
        reconfigureSubgroup.setName(name);

        const std::string subgroupPath = currentPath.empty() ? name : currentPath + "/" + name;

        for (auto& feature : features) {
            if (feature) {
                traverseNode(feature, liveSubgroup, reconfigureSubgroup, subgroupPath);
            }
        }

        if (liveSubgroup.size() > 0) {
            liveContainer.add(liveSubgroup);
        }
        if (reconfigureSubgroup.size() > 0) {
            reconfigureContainer.add(reconfigureSubgroup);
        }
        return;
    }

    if (interfaceType == intfICommand) {
        // Action nodes (TriggerSoftware, UserSetSave, ...) aren't exposed as
        // bindable parameters.
        return;
    }

    const std::string displayName = node->GetDisplayName().c_str();
    const std::string parameterPath = currentPath.empty() ? displayName : currentPath + "/" + displayName;

    cacheNode(parameterPath, node);

    if (node->IsSelector()) {
        selectorParameterPaths.insert(parameterPath);
    }

    bool writableNow = false;
    try {
        writableNow = IsWritable(node);
    } catch (const Spinnaker::Exception&) {
        writableNow = false;
    }

    if (!controlsClassified) {
        // Baseline snapshot for the one-time pre/post-stream classification
        // probe in classifyControlGroups(). Nodes that are only non-writable
        // because an Auto mode currently governs them (e.g. ExposureTime while
        // ExposureAuto != Off) are still exposed here and land in Live Controls
        // by default; their live enabled/disabled state is handled separately
        // by isParameterCurrentlyWritable().
        preStreamWritableSnapshot[parameterPath] = writableNow;
    }

    ofxSpinnakerControlClass controlClass = ofxSpinnakerControlClass::RealTime;
    auto classIt = nodeClassification.find(parameterPath);
    if (classIt != nodeClassification.end()) {
        controlClass = classIt->second;
    }

    ofParameterGroup& container = (controlClass == ofxSpinnakerControlClass::StopCaptureRequired)
                                       ? reconfigureContainer
                                       : liveContainer;

    try {
        switch (interfaceType) {
            case intfIEnumeration: {
                CEnumerationPtr enumeration = static_cast<CEnumerationPtr>(node);
                if (!enumeration) {
                    return;
                }

                NodeList_t entries;
                enumeration->GetEntries(entries);

                std::vector<std::string> names;
                std::map<int, int64_t> valueMap;

                const int64_t currentValue = enumeration->GetIntValue();
                int currentIndex = 0;
                int index = 0;

                for (auto& entry : entries) {
                    if (!entry || !IsAvailable(entry) || !IsReadable(entry)) {
                        continue;
                    }

                    CEnumEntryPtr enumEntry = static_cast<CEnumEntryPtr>(entry);
                    if (!enumEntry) {
                        continue;
                    }

                    const std::string symbolic = enumEntry->GetSymbolic().c_str();
                    const int64_t value = enumEntry->GetValue();

                    names.push_back(symbolic);
                    valueMap[index] = value;
                    if (value == currentValue) {
                        currentIndex = index;
                    }
                    ++index;
                }

                if (names.empty()) {
                    return;
                }

                enumDisplayNames[parameterPath] = names;
                enumValueMaps[parameterPath] = valueMap;

                ofParameter<int> parameter;
                parameter.set(displayName, currentIndex, 0, static_cast<int>(names.size() - 1));
                container.add(parameter);
                ofParameter<int>& stored = container.get<int>(displayName);
                parameterPaths[&stored] = parameterPath;
                pathToParameter[parameterPath] = &stored;
                break;
            }
            case intfIFloat: {
                CFloatPtr floatNode = static_cast<CFloatPtr>(node);
                if (!floatNode) {
                    return;
                }

                ofParameter<float> parameter;
                parameter.set(displayName,
                              static_cast<float>(floatNode->GetValue()),
                              static_cast<float>(floatNode->GetMin()),
                              static_cast<float>(floatNode->GetMax()));
                container.add(parameter);
                ofParameter<float>& stored = container.get<float>(displayName);
                parameterPaths[&stored] = parameterPath;
                pathToParameter[parameterPath] = &stored;
                break;
            }
            case intfIInteger: {
                CIntegerPtr intNode = static_cast<CIntegerPtr>(node);
                if (!intNode) {
                    return;
                }

                // ofParameter<int> is a 32-bit signed int, but GenICam integer
                // nodes are 64-bit - some (IP-address/register-style fields
                // like GevSCDA) legitimately use the full unsigned 32-bit
                // range or beyond. Narrowing that without clamping first
                // silently overflows into a garbage (often negative) min/max,
                // which is exactly what previously slipped through and made
                // an ImGui slider try to render with out-of-range bounds.
                constexpr int64_t kInt32Min = static_cast<int64_t>(std::numeric_limits<int>::min());
                constexpr int64_t kInt32Max = static_cast<int64_t>(std::numeric_limits<int>::max());
                const int clampedMin = static_cast<int>(std::clamp(intNode->GetMin(), kInt32Min, kInt32Max));
                const int clampedMax = static_cast<int>(std::clamp(intNode->GetMax(), kInt32Min, kInt32Max));
                const int clampedValue = static_cast<int>(std::clamp(intNode->GetValue(),
                                                                       static_cast<int64_t>(clampedMin),
                                                                       static_cast<int64_t>(clampedMax)));

                ofParameter<int> parameter;
                parameter.set(displayName, clampedValue, clampedMin, clampedMax);
                container.add(parameter);
                ofParameter<int>& stored = container.get<int>(displayName);
                parameterPaths[&stored] = parameterPath;
                pathToParameter[parameterPath] = &stored;
                break;
            }
            case intfIBoolean: {
                CBooleanPtr boolNode = static_cast<CBooleanPtr>(node);
                if (!boolNode) {
                    return;
                }

                ofParameter<bool> parameter;
                parameter.set(displayName, boolNode->GetValue());
                container.add(parameter);
                ofParameter<bool>& stored = container.get<bool>(displayName);
                parameterPaths[&stored] = parameterPath;
                pathToParameter[parameterPath] = &stored;
                break;
            }
            case intfIString: {
                CStringPtr stringNode = static_cast<CStringPtr>(node);
                if (!stringNode) {
                    return;
                }

                ofParameter<std::string> parameter;
                parameter.set(displayName, stringNode->GetValue().c_str());
                container.add(parameter);
                ofParameter<std::string>& stored = container.get<std::string>(displayName);
                parameterPaths[&stored] = parameterPath;
                pathToParameter[parameterPath] = &stored;
                break;
            }
            default:
                break;
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogWarning("ofxSpinnakerCamera") << "Failed to attach node " << displayName << ": " << e.what();
    }
}

void ofxSpinnakerCamera::onParameterChanged(ofAbstractParameter& parameter) {
    if (!camera || !camera->IsValid()) {
        return;
    }

    const ofAbstractParameter* parameterPtr = &parameter;
    auto pathIt = parameterPaths.find(parameterPtr);
    if (pathIt == parameterPaths.end()) {
        return;
    }

    const std::string& path = pathIt->second;
    auto nodeIt = writableNodes.find(path);
    if (nodeIt == writableNodes.end()) {
        return;
    }

    NodePtr node = nodeIt->second;
    if (!node) {
        return;
    }

    // Parameters are now exposed even when currently non-writable (so an
    // Auto-governed control like ExposureTime still shows up, just disabled,
    // rather than vanishing from the tree) - but that means a saved config
    // can legitimately reference a control that isn't writable right now
    // (Auto still on) or ever (a read-only GEV/transport-layer status field).
    // Check first instead of attempting SetValue() and catching the
    // exception: it's cheap, and avoids treating an entirely expected
    // situation as a warning-worthy failure. An interactive GUI edit
    // shouldn't reach here at all for a non-writable control (widgets should
    // be disabled via isParameterCurrentlyWritable()), so still warn in that
    // case - it points at a real GUI bug rather than a stale config value.
    if (!IsWritable(node)) {
        if (loadingConfiguration) {
            ofLogVerbose("ofxSpinnakerCamera") << "Skipping non-writable parameter '" << parameter.getName()
                                                << "' while restoring configuration.";
        } else {
            ofLogWarning("ofxSpinnakerCamera") << "Ignoring edit to '" << parameter.getName()
                                                << "': node is not currently writable.";
        }
        return;
    }

    bool success = false;

    try {
        switch (node->GetPrincipalInterfaceType()) {
            case intfIEnumeration: {
                ofParameter<int>& intParam = parameter.cast<int>();
                success = applyEnumeration(path, node, intParam);
                break;
            }
            case intfIFloat: {
                ofParameter<float>& floatParam = parameter.cast<float>();
                success = applyFloat(path, node, floatParam);
                break;
            }
            case intfIInteger: {
                ofParameter<int>& intParam = parameter.cast<int>();
                success = applyInteger(path, node, intParam);
                break;
            }
            case intfIBoolean: {
                ofParameter<bool>& boolParam = parameter.cast<bool>();
                success = applyBoolean(path, node, boolParam);
                break;
            }
            case intfIString: {
                ofParameter<std::string>& stringParam = parameter.cast<std::string>();
                success = applyString(path, node, stringParam);
                break;
            }
            default:
                break;
        }
    } catch (const std::exception& e) {
        logNodeFailure(parameter.getName(), e.what(), "apply value");
        success = false;
    }

    if (!success) {
        ofLogWarning("ofxSpinnakerCamera") << "Parameter update failed for " << parameter.getName();
        return;
    }

    if (!loadingConfiguration) {
        markConfigurationDirty();
    }
}

bool ofxSpinnakerCamera::isStopCaptureRequired(const std::string& path) const {
    auto it = nodeClassification.find(path);
    return it != nodeClassification.end() && it->second == ofxSpinnakerControlClass::StopCaptureRequired;
}

void ofxSpinnakerCamera::requestDeferredRebuild() {
    pendingRebuildRequested = true;
    if (loadingConfiguration) {
        pendingRebuildSkipConfigReload = true;
    }
}

bool ofxSpinnakerCamera::applyEnumeration(const std::string& path, NodePtr node, ofParameter<int>& parameter) {
    CEnumerationPtr enumeration = static_cast<CEnumerationPtr>(node);
    if (!enumeration) {
        return false;
    }

    const auto valueMapIt = enumValueMaps.find(path);
    if (valueMapIt == enumValueMaps.end()) {
        return false;
    }

    const auto valueIt = valueMapIt->second.find(parameter.get());
    if (valueIt == valueMapIt->second.end()) {
        ofLogWarning("ofxSpinnakerCamera") << "Enumeration value out of range for " << path;
        return false;
    }

    const auto setter = [&]() {
        enumeration->SetIntValue(valueIt->second);
    };

    if (isStopCaptureRequired(path)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isStopCaptureRequired(path)) {
        requestDeferredRebuild();
    }
    return true;
}

bool ofxSpinnakerCamera::applyFloat(const std::string& path, NodePtr node, ofParameter<float>& parameter) {
    CFloatPtr floatNode = static_cast<CFloatPtr>(node);
    if (!floatNode) {
        return false;
    }

    const auto setter = [&]() {
        floatNode->SetValue(parameter.get());
    };

    if (isStopCaptureRequired(path)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isStopCaptureRequired(path)) {
        requestDeferredRebuild();
    }
    return true;
}

bool ofxSpinnakerCamera::applyInteger(const std::string& path, NodePtr node, ofParameter<int>& parameter) {
    CIntegerPtr intNode = static_cast<CIntegerPtr>(node);
    if (!intNode) {
        return false;
    }

    const auto setter = [&]() {
        intNode->SetValue(parameter.get());
    };

    if (isStopCaptureRequired(path)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isStopCaptureRequired(path)) {
        requestDeferredRebuild();
    }
    return true;
}

bool ofxSpinnakerCamera::applyBoolean(const std::string& path, NodePtr node, ofParameter<bool>& parameter) {
    CBooleanPtr boolNode = static_cast<CBooleanPtr>(node);
    if (!boolNode) {
        return false;
    }

    const auto setter = [&]() {
        boolNode->SetValue(parameter.get());
    };

    if (isStopCaptureRequired(path)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << (parameter.get() ? "true" : "false");

    if (selectorParameterPaths.count(path) > 0 || isStopCaptureRequired(path)) {
        requestDeferredRebuild();
    }
    return true;
}

bool ofxSpinnakerCamera::applyString(const std::string& path, NodePtr node, ofParameter<std::string>& parameter) {
    CStringPtr stringNode = static_cast<CStringPtr>(node);
    if (!stringNode) {
        return false;
    }

    const auto setter = [&]() {
        stringNode->SetValue(parameter.get().c_str());
    };

    if (isStopCaptureRequired(path)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isStopCaptureRequired(path)) {
        requestDeferredRebuild();
    }
    return true;
}

void ofxSpinnakerCamera::cacheNode(const std::string& path, NodePtr node) {
    if (!path.empty() && node) {
        writableNodes[path] = node;
        // Stable GenICam identifier (e.g. "ExposureTime"), as opposed to the
        // display name used for the path above - see buildNaturalControls().
        genicamNameToPath[node->GetName().c_str()] = path;
    }
}

void ofxSpinnakerCamera::ensureTextureMatches(const ofPixels& pixelsRef) {
    ofTexture& tex = texturePlanes[0];
    // Format-aware (not hardcoded to GL_RGB) since setPixelFormat() can
    // switch the delivered format to grayscale - comparing against a fixed
    // GL_RGB would otherwise force a reallocation every single frame once
    // running in grayscale mode.
    if (!tex.isAllocated() ||
        tex.getWidth() != pixelsRef.getWidth() ||
        tex.getHeight() != pixelsRef.getHeight() ||
        tex.getTextureData().glInternalFormat != ofGetGLInternalFormat(pixelsRef)) {
        tex.allocate(pixelsRef);
    }
}

void ofxSpinnakerCamera::logNodeFailure(const std::string& nodeName,
                                        const std::string& message,
                                        const std::string& action) {
    ofLogWarning("ofxSpinnakerCamera") << "Node '" << nodeName << "' failed during " << action << ": " << message;
}

void ofxSpinnakerCamera::attachParameterListener() {
    if (listenerAttached) {
        return;
    }
    parameterListener = rootParameters.parameterChangedE().newListener(this, &ofxSpinnakerCamera::onParameterChanged);
    listenerAttached = true;
}

void ofxSpinnakerCamera::rebuildParameters() {
    if (rebuildingParameters) {
        return;
    }

    rebuildingParameters = true;

    if (listenerAttached) {
        parameterListener.unsubscribe();
        listenerAttached = false;
    }

    buildParameterTree();
    attachParameterListener();

    if (configLoaded && !skipConfigReload && !cachedConfiguration.is_null() && cachedConfiguration.contains("parameters")) {
        loadingConfiguration = true;
        applyAllParametersFromJson(cachedConfiguration["parameters"]);
        loadingConfiguration = false;
    }

    parameterRevision.fetch_add(1, std::memory_order_acq_rel);
    guiRefreshRequested.store(true, std::memory_order_release);

    rebuildingParameters = false;
}

void ofxSpinnakerCamera::stopAcquisitionForCriticalChange(const std::function<void()>& fn) {
    const bool restart = isStreaming();

    if (restart) {
        ofLogNotice("ofxSpinnakerCamera") << "Temporarily stopping acquisition on " << serialNumber
                                           << " for a control that requires the stream to be stopped.";
        stopStreaming();
    }

    try {
        fn();
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Parameter update requiring stopped capture failed: " << e.what();
    }

    if (restart) {
        startStreaming();
    }
}
