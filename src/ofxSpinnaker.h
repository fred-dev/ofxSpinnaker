#pragma once

#include "ofMain.h"
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include "ofThread.h"
#include "ofJson.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ofxSpinnakerCamera;

class ofxSpinnaker {
public:
    ofxSpinnaker();
    ~ofxSpinnaker();

    bool setup(const std::string& configurationDirectory = "");
    void shutdown();

    void setConfigurationDirectory(const std::string& directory);

    void update();

    void draw(int cameraIndex, float x, float y) const;
    void draw(int cameraIndex, float x, float y, float width, float height) const;

    size_t getNumCameras() const;
    std::shared_ptr<ofxSpinnakerCamera> getCamera(size_t index) const;

    const ofParameterGroup& getCameraParameterGroup(size_t index) const;

    void refreshCameraList();

private:
    void discoverCameras();

    Spinnaker::SystemPtr system;
    std::vector<std::shared_ptr<ofxSpinnakerCamera>> cameras;
    mutable std::mutex cameraMutex;
    std::string configurationDirectory;
};


class ofxSpinnakerCamera : public ofThread {
public:
    explicit ofxSpinnakerCamera(Spinnaker::CameraPtr cameraPtr);
    ~ofxSpinnakerCamera() override;

    bool setup();
    void shutdown();

    void startStreaming();
    void stopStreaming();
    bool isStreaming() const;

    void update();
    void draw(float x, float y) const;
    void draw(float x, float y, float width, float height) const;

    ofTexture& getTexture();
    ofPixels& getPixels();

    const std::string& getSerialNumber() const;
    const std::string& getDeviceDisplayName() const;

    ofParameterGroup& getParameterRoot();
    const std::unordered_map<std::string, std::vector<std::string>>& getEnumDisplayNames() const;
    uint64_t getParameterRevision() const;
    bool consumeGuiRefreshFlag();
    void setConfigurationDirectory(const std::string& directory);

private:
    using NodePtr = Spinnaker::GenApi::CNodePtr;

    void threadedFunction() override;

    void buildParameterTree();
    void buildInfoGroup(Spinnaker::GenApi::INodeMap& nodeMap);
    void traverseNode(NodePtr node,
                      ofParameterGroup& container,
                      const std::string& sectionName,
                      unsigned int depth = 0,
                      const std::string& currentPath = "");
    void attachParameterListener();
    void rebuildParameters();

    void onParameterChanged(ofAbstractParameter& parameter);
    bool applyEnumeration(const std::string& path, NodePtr node, ofParameter<int>& parameter);
    bool applyFloat(const std::string& path, NodePtr node, ofParameter<float>& parameter);
    bool applyInteger(const std::string& path, NodePtr node, ofParameter<int>& parameter);
    bool applyBoolean(const std::string& path, NodePtr node, ofParameter<bool>& parameter);
    bool applyString(const std::string& path, NodePtr node, ofParameter<std::string>& parameter);

    void cacheNode(const std::string& path, NodePtr node);

    void ensureTextureMatches(const ofPixels& pixels);

    void logNodeFailure(const std::string& nodeName, const std::string& message, const std::string& action);

    void stopAcquisitionForCriticalChange(const std::function<void()>& fn);

    bool isCriticalNode(const std::string& nodeName) const;

    void loadConfiguration();
    void saveConfiguration();
    ofJson captureConfigurationSnapshot() const;
    void collectParameters(const ofParameterGroup& group,
                           const std::string& prefix,
                           ofJson& collection) const;
    void applyConfigurationToGroup(ofParameterGroup& group,
                                   const std::string& prefix,
                                   const ofJson& values);
    void markConfigurationDirty();

    Spinnaker::CameraPtr camera;
    Spinnaker::ImageProcessor processor;

    mutable std::mutex frameMutex;
    ofPixels pixels;
    ofTexture texture;
    bool newFrameAvailable = false;
    bool streaming = false;

    ofParameterGroup rootParameters;
    ofParameterGroup liveParameters;
    ofParameterGroup reconfigureParameters;
    ofParameterGroup infoParameters;
    ofParameterGroup advancedParameters;

    std::unordered_map<std::string, NodePtr> writableNodes;
    std::unordered_map<std::string, std::vector<std::string>> enumDisplayNames;
    std::unordered_map<std::string, std::map<int, int64_t>> enumValueMaps;
    std::unordered_map<const ofAbstractParameter*, std::string> parameterPaths;
    std::unordered_set<std::string> selectorParameterPaths;

    ofEventListener parameterListener;
    bool listenerAttached = false;
    bool rebuildingParameters = false;
    bool loadingConfiguration = false;
    bool skipConfigReload = false;
    std::atomic<uint64_t> parameterRevision{0};
    std::atomic<bool> guiRefreshRequested{false};
    std::atomic<bool> configDirty{false};

    std::string serialNumber;
    std::string deviceDisplayName;
    std::string configDirectory;
    std::string configFilePath;
    bool configLoaded = false;
    uint64_t lastConfigSaveTime = 0;
    ofJson cachedConfiguration;
};
