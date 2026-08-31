#pragma once

#include "ofMain.h"
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
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


// A control's real-time behavior, discovered empirically from the camera rather
// than guessed from its name: whether it can be changed while the camera is
// streaming, or whether the stream must briefly be stopped to change it.
enum class ofxSpinnakerControlClass {
    RealTime,
    StopCaptureRequired
};

class ofxSpinnakerCamera : public Spinnaker::ImageEventHandler {
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

    // Live query for GUI layers: is this parameter's underlying GenICam node
    // currently writable? Backed by a fresh IsWritable()/IsAvailable() check on
    // the cached node, so it reflects Auto/Manual dependencies (e.g. ExposureAuto
    // != Off disabling ExposureTime) the instant they change, without needing a
    // full parameter-tree rebuild.
    bool isParameterCurrentlyWritable(const ofAbstractParameter& parameter) const;

    // Returns the enum's display-name entries (in the same order as the
    // ofParameter<int>'s underlying index) if this parameter is an
    // enumeration, or nullptr otherwise. Pointer-keyed so callers never need
    // to know the addon's internal GenICam node-path naming.
    const std::vector<std::string>* getEnumEntryNames(const ofAbstractParameter& parameter) const;

private:
    using NodePtr = Spinnaker::GenApi::CNodePtr;

    void OnImageEvent(Spinnaker::ImagePtr image) override;

    void buildParameterTree();
    void buildInfoGroup(Spinnaker::GenApi::INodeMap& nodeMap);
    void traverseNode(NodePtr node,
                      ofParameterGroup& liveContainer,
                      ofParameterGroup& reconfigureContainer,
                      const std::string& currentPath = "");
    void configureStreamBuffering();
    void classifyControlGroups();
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

    bool isStopCaptureRequired(const std::string& path) const;

    void resetGevHeartbeatIfPresent();

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

    // Walk only Live/Reconfigure controls for JSON persistence, skipping the
    // read-only Info group (already captured under the "device" key) and,
    // crucially, not folding the "Live Controls"/"Reconfigure Controls" bucket
    // names into the path - those buckets can move a control between them on
    // reclassification, but the underlying GenICam node path (and therefore
    // the JSON key) must stay stable.
    void collectAllParameters(ofJson& collection) const;
    void applyAllParametersFromJson(const ofJson& values);

    Spinnaker::CameraPtr camera;
    Spinnaker::ImageProcessor processor;
    bool imageEventHandlerRegistered = false;

    mutable std::mutex frameMutex;
    ofPixels pixels;
    ofTexture texture;
    bool newFrameAvailable = false;
    bool streaming = false;

    ofParameterGroup rootParameters;
    ofParameterGroup liveParameters;
    ofParameterGroup reconfigureParameters;
    ofParameterGroup infoParameters;

    std::unordered_map<std::string, NodePtr> writableNodes;
    std::unordered_map<std::string, std::vector<std::string>> enumDisplayNames;
    std::unordered_map<std::string, std::map<int, int64_t>> enumValueMaps;
    std::unordered_map<const ofAbstractParameter*, std::string> parameterPaths;
    std::unordered_set<std::string> selectorParameterPaths;

    // Discovered once per camera (see classifyControlGroups()): which nodes
    // actually require the stream to be stopped to change, versus which remain
    // writable while streaming. Persists across rebuilds so re-classification
    // only has to happen for genuinely new node paths.
    std::unordered_map<std::string, ofxSpinnakerControlClass> nodeClassification;
    std::unordered_map<std::string, bool> preStreamWritableSnapshot;
    bool controlsClassified = false;

    ofEventListener parameterListener;
    bool listenerAttached = false;
    bool rebuildingParameters = false;
    bool pendingRebuildRequested = false;
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
