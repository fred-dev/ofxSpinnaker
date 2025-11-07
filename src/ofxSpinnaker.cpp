#include "ofxSpinnaker.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "ofFileUtils.h"
#include "ofUtils.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

namespace {
constexpr uint64_t kImageTimeoutMs = 2000; // milliseconds

std::string toLower(const std::string& value) {
    std::string copy = value;
    std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return copy;
}

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

std::string classifySectionFromName(const std::string& name) {
    const std::string lower = toLower(name);

    if (lower.find("device") != std::string::npos ||
        lower.find("information") != std::string::npos ||
        lower.find("status") != std::string::npos ||
        lower.find("diagnostic") != std::string::npos) {
        return "info";
    }

    if (lower.find("exposure") != std::string::npos ||
        lower.find("balance") != std::string::npos ||
        lower.find("gamma") != std::string::npos ||
        lower.find("gain") != std::string::npos ||
        lower.find("black") != std::string::npos ||
        lower.find("sharpness") != std::string::npos ||
        lower.find("tone") != std::string::npos ||
        lower.find("color") != std::string::npos) {
        return "live";
    }

    if (lower.find("acquisition") != std::string::npos ||
        lower.find("trigger") != std::string::npos ||
        lower.find("sequencer") != std::string::npos ||
        lower.find("gige") != std::string::npos ||
        lower.find("stream") != std::string::npos ||
        lower.find("imageformat") != std::string::npos ||
        lower.find("lut") != std::string::npos ||
        lower.find("transport") != std::string::npos) {
        return "reconfigure";
    }

    return "advanced";
}

const std::unordered_set<std::string>& criticalNodeNames() {
    static const std::unordered_set<std::string> critical = {
        "pixelformat",
        "width",
        "height",
        "offsetx",
        "offsety",
        "binninghorizontal",
        "binningvertical",
        "decimationhorizontal",
        "decimationvertical",
        "acquisitionmode",
        "acquisitionframerateenable",
        "acquisitionframerate",
        "devicelinkthroughputlimit",
        "reversex",
        "reversey"
    };
    return critical;
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

    buildParameterTree();
    attachParameterListener();
    loadConfiguration();
    parameterRevision.store(1, std::memory_order_relaxed);
    guiRefreshRequested.store(true, std::memory_order_release);

    startThread();
    startStreaming();

    ofLogNotice("ofxSpinnakerCamera") << "Camera " << serialNumber << " initialized."
                                       << " Device: " << deviceDisplayName;

    return true;
}

void ofxSpinnakerCamera::shutdown() {
    stopStreaming();
    waitForThread(true);

    if (listenerAttached) {
        parameterListener.unsubscribe();
        listenerAttached = false;
    }

    if (camera && camera->IsValid()) {
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
    {
        std::unique_lock<std::mutex> lock(frameMutex);
        if (newFrameAvailable) {
            ensureTextureMatches(pixels);
            texture.loadData(pixels);
            newFrameAvailable = false;
        }
    }

    if (configDirty.load(std::memory_order_acquire) && !loadingConfiguration) {
        const uint64_t now = ofGetElapsedTimeMillis();
        if (now - lastConfigSaveTime > 100) {
            saveConfiguration();
        }
    }
}

void ofxSpinnakerCamera::draw(float x, float y) const {
    if (texture.isAllocated()) {
        texture.draw(x, y);
    }
}

void ofxSpinnakerCamera::draw(float x, float y, float width, float height) const {
    if (texture.isAllocated()) {
        texture.draw(x, y, width, height);
    }
}

ofTexture& ofxSpinnakerCamera::getTexture() {
    return texture;
}

ofPixels& ofxSpinnakerCamera::getPixels() {
    return pixels;
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
            applyConfigurationToGroup(rootParameters, "", json["parameters"]);
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
        collectParameters(rootParameters, "", cachedConfiguration["parameters"]);
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
    collectParameters(rootParameters, "", params);
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

void ofxSpinnakerCamera::threadedFunction() {
    while (isThreadRunning()) {
        if (!streaming) {
            ofSleepMillis(10);
            continue;
        }

        try {
            ImagePtr nextImage = camera->GetNextImage(kImageTimeoutMs);

            if (!nextImage || nextImage->IsIncomplete()) {
                if (nextImage && nextImage->IsIncomplete()) {
                    ofLogWarning("ofxSpinnakerCamera") << "Incomplete image on " << serialNumber
                                                       << " - status " << nextImage->GetImageStatus();
                }
                continue;
            }

            ImagePtr converted = processor.Convert(nextImage, PixelFormat_RGB8);

            std::unique_lock<std::mutex> lock(frameMutex);
            pixels.setFromPixels(static_cast<unsigned char*>(converted->GetData()),
                                 converted->GetWidth(),
                                 converted->GetHeight(),
                                 OF_IMAGE_COLOR);
            newFrameAvailable = true;
            lock.unlock();

            nextImage->Release();
        } catch (const Spinnaker::Exception& e) {
            if (streaming) {
                ofLogWarning("ofxSpinnakerCamera") << "Image acquisition error on " << serialNumber << ": " << e.what();
            }
            ofSleepMillis(5);
        }
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
    advancedParameters.clear();
    writableNodes.clear();
    enumDisplayNames.clear();
    enumValueMaps.clear();
    parameterPaths.clear();
    selectorParameterPaths.clear();

    rootParameters.setName(deviceDisplayName.empty() ? "Camera" : deviceDisplayName);
    infoParameters.setName("Info");
    liveParameters.setName("Live Controls");
    reconfigureParameters.setName("Reconfigure Controls");
    advancedParameters.setName("Advanced Controls");

    rootParameters.add(infoParameters);
    rootParameters.add(liveParameters);
    rootParameters.add(reconfigureParameters);
    rootParameters.add(advancedParameters);

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
            if (!feature) {
                continue;
            }
            const std::string section = classifySectionFromName(feature->GetName().c_str());
            ofParameterGroup* target = nullptr;
            if (section == "info") {
                target = &infoParameters;
            } else if (section == "live") {
                target = &liveParameters;
            } else if (section == "reconfigure") {
                target = &reconfigureParameters;
            } else {
                target = &advancedParameters;
            }

            std::string basePath = target ? target->getName() : section;
            traverseNode(feature, *target, section, 0, basePath);
        }
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Failed to traverse node map: " << e.what();
    }
}

void ofxSpinnakerCamera::buildInfoGroup(INodeMap& nodeMap) {
    infoParameters.clear();
    infoParameters.setName("Camera Info");

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

void ofxSpinnakerCamera::traverseNode(NodePtr node,
                                      ofParameterGroup& container,
                                      const std::string& sectionName,
                                      unsigned int depth,
                                      const std::string& currentPath) {
    if (!node || !IsReadable(node)) {
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

        ofParameterGroup subgroup;
        subgroup.setName(category->GetDisplayName().c_str());

        const std::string subgroupPath = currentPath.empty()
                                             ? subgroup.getName()
                                             : currentPath + "/" + subgroup.getName();

        for (auto& feature : features) {
            if (feature) {
                traverseNode(feature, subgroup, sectionName, depth + 1, subgroupPath);
            }
        }

        if (subgroup.size() > 0) {
            container.add(subgroup);
        }
        return;
    }

    if (!IsWritable(node)) {
        return;
    }

    const std::string displayName = node->GetDisplayName().c_str();
    const std::string parameterPath = currentPath.empty() ? displayName : currentPath + "/" + displayName;

    cacheNode(parameterPath, node);

    if (node->IsSelector()) {
        selectorParameterPaths.insert(parameterPath);
    }

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
                break;
            }
            case intfIInteger: {
                CIntegerPtr intNode = static_cast<CIntegerPtr>(node);
                if (!intNode) {
                    return;
                }

                ofParameter<int> parameter;
                parameter.set(displayName,
                              static_cast<int>(intNode->GetValue()),
                              static_cast<int>(intNode->GetMin()),
                              static_cast<int>(intNode->GetMax()));
                container.add(parameter);
                ofParameter<int>& stored = container.get<int>(displayName);
                parameterPaths[&stored] = parameterPath;
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

    const std::string name = parameter.getName();
    if (isCriticalNode(name)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isCriticalNode(name)) {
        skipConfigReload = true;
        rebuildParameters();
        skipConfigReload = false;
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

    const std::string name = parameter.getName();
    if (isCriticalNode(name)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isCriticalNode(name)) {
        skipConfigReload = true;
        rebuildParameters();
        skipConfigReload = false;
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

    const std::string name = parameter.getName();
    if (isCriticalNode(name)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isCriticalNode(name)) {
        skipConfigReload = true;
        rebuildParameters();
        skipConfigReload = false;
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

    const std::string name = parameter.getName();
    if (isCriticalNode(name)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << (parameter.get() ? "true" : "false");

    if (selectorParameterPaths.count(path) > 0 || isCriticalNode(name)) {
        skipConfigReload = true;
        rebuildParameters();
        skipConfigReload = false;
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

    const std::string name = parameter.getName();
    if (isCriticalNode(name)) {
        stopAcquisitionForCriticalChange(setter);
    } else {
        setter();
    }

    ofLogNotice("ofxSpinnakerCamera") << path << " -> " << parameter.get();

    if (selectorParameterPaths.count(path) > 0 || isCriticalNode(name)) {
        skipConfigReload = true;
        rebuildParameters();
        skipConfigReload = false;
    }
    return true;
}

void ofxSpinnakerCamera::cacheNode(const std::string& path, NodePtr node) {
    if (!path.empty() && node) {
        writableNodes[path] = node;
    }
}

void ofxSpinnakerCamera::ensureTextureMatches(const ofPixels& pixelsRef) {
    if (!texture.isAllocated() ||
        texture.getWidth() != pixelsRef.getWidth() ||
        texture.getHeight() != pixelsRef.getHeight() ||
        texture.getTextureData().glInternalFormat != GL_RGB) {
        texture.allocate(pixelsRef);
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
        applyConfigurationToGroup(rootParameters, "", cachedConfiguration["parameters"]);
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
                                           << " for critical parameter change.";
        stopStreaming();
    }

    try {
        fn();
    } catch (const Spinnaker::Exception& e) {
        ofLogError("ofxSpinnakerCamera") << "Critical parameter update failed: " << e.what();
    }

    if (restart) {
        startStreaming();
    }
}

bool ofxSpinnakerCamera::isCriticalNode(const std::string& nodeName) const {
    const std::string lower = toLower(nodeName);
    return criticalNodeNames().count(lower) > 0;
}

