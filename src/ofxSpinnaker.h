#pragma once

#include "ofMain.h"
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include "ofJson.h"

#include <atomic>
#include <functional>
#include <initializer_list>
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

// ofxSpinnakerCamera doubles as an ofBaseVideoGrabber/ofBaseVideoDraws (the
// same interfaces ofVideoGrabber itself implements), so it can be used as a
// drop-in replacement in code written against those interfaces, or just by
// matching call sites directly: update()/isFrameNew()/getPixels()/
// getTexture()/draw() all behave the same way regardless of which capture
// source is behind them. Camera selection/initialization is still Spinnaker-
// specific (real setup happens through ofxSpinnaker's discovery, not the
// interface's setup(int,int) below, which exists only to satisfy the
// contract - see its comment).
class ofxSpinnakerCamera : public Spinnaker::ImageEventHandler,
                           public ofBaseVideoGrabber,
                           public ofBaseVideoDraws {
public:
    explicit ofxSpinnakerCamera(Spinnaker::CameraPtr cameraPtr);
    ~ofxSpinnakerCamera() override;

    bool setup();
    void shutdown();

    void startStreaming();
    void stopStreaming();
    bool isStreaming() const;

    void update() override;
    void draw(float x, float y) const override;
    void draw(float x, float y, float width, float height) const override;

    ofTexture& getTexture() override;
    const ofTexture& getTexture() const override;
    ofPixels& getPixels() override;
    const ofPixels& getPixels() const override;

    // --- ofBaseVideo / ofBaseVideoGrabber / ofBaseVideoDraws contract ---
    // (grouped here rather than interleaved with the addon's own API, since
    // they exist for interchangeability with other capture sources, not as
    // primary entry points)
    bool isFrameNew() const override;
    void close() override;
    bool isInitialized() const override;
    bool setPixelFormat(ofPixelFormat pixelFormat) override;
    ofPixelFormat getPixelFormat() const override;

    std::vector<ofVideoDevice> listDevices() const override;
    // Compatibility stub: real initialization happens via ofxSpinnaker's
    // discovery + this class's own no-arg setup(), already called by the
    // time application code ever holds a reference to this object. If
    // called on an already-initialized camera this is a no-op returning
    // true; w/h are otherwise unused (matches ofBaseVideoGrabber's own
    // "may be treated as a hint" contract, but ofFramework rarely calls this
    // dynamically since it's only reached through the polymorphic interface).
    bool setup(int w, int h) override;
    float getWidth() const override;
    float getHeight() const override;

    void setUseTexture(bool useTex) override;
    bool isUsingTexture() const override;
    std::vector<ofTexture>& getTexturePlanes() override;
    const std::vector<ofTexture>& getTexturePlanes() const override;

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

    // "Natural Camera" simplified tiers: curated views built by aliasing
    // already-constructed parameters from the raw tree (via
    // ofParameterGroup::add(), which shares the same underlying value -
    // edits through either view stay in sync) rather than duplicating them.
    // Looked up by each node's stable GenICam name (e.g. "ExposureTime"),
    // not its display name, since display names vary by camera/vendor even
    // for standard SFNC features (confirmed: this camera's
    // AcquisitionFrameRateEnable displays as "Acquisition Frame Rate Control
    // Enabled", not the SFNC default). Anything a given camera doesn't have
    // is silently skipped.
    void buildNaturalControls();
    void buildAdvancedControls();
    ofAbstractParameter* findByGenICamName(const std::string& genicamName) const;
    // Tries each candidate name in order, aliasing the first that exists on
    // this camera - used where the exact node name isn't nailed down by SFNC
    // (e.g. Blackfly-lineage Hue/Saturation controls aren't part of the
    // documented GenICam surface, so the precise spelling is a best guess).
    void addAliasToGroup(ofParameterGroup& target, std::initializer_list<std::string> candidateNames);
    void buildFilteredPixelFormatControl();
    void updateMeasuredFps();
    void classifyControlGroups();
    void attachParameterListener();
    void rebuildParameters();
    void requestDeferredRebuild();

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
    // Single-element by construction (resized to 1 in the constructor) so
    // getTexturePlanes() always has a valid entry to reference, even before
    // the first frame arrives. getTexture() is just texturePlanes[0].
    std::vector<ofTexture> texturePlanes;
    bool newFrameAvailable = false;
    bool streaming = false;

    // ofBaseVideo contract state, unrelated to the addon's own GenICam state.
    bool frameIsNew = false;
    bool initializedFlag = false;
    // Set from setPixelFormat() (main thread) and read from OnImageEvent
    // (Spinnaker's own acquisition thread) - atomic rather than sharing
    // frameMutex since it's a single small value, not part of the
    // pixel/texture hand-off.
    std::atomic<ofPixelFormat> outputPixelFormat{OF_PIXELS_RGB};
    bool useTexture = true;

    // Only ever touched from OnImageEvent (Spinnaker's own acquisition
    // thread, called serially), so these don't need frameMutex.
    uint64_t lastIncompleteImageLogTime = 0;
    uint32_t incompleteImageCount = 0;

    // Delivered-frame counter for the measured "Actual FPS" display (see
    // updateMeasuredFps()). frameCount is incremented in OnImageEvent
    // (Spinnaker's acquisition thread) and read/reset from update() (main
    // thread), so it shares frameMutex with the pixel buffer rather than
    // needing a separate lock.
    uint64_t frameCount = 0;
    uint64_t fpsWindowStartMs = 0;
    float measuredFps = 0.0f;

    ofParameterGroup rootParameters;
    ofParameterGroup liveParameters;
    ofParameterGroup reconfigureParameters;
    ofParameterGroup infoParameters;
    ofParameterGroup naturalParameters;
    ofParameterGroup advancedParameters;
    ofParameter<std::string> actualFpsParameter;

    std::unordered_map<std::string, NodePtr> writableNodes;
    std::unordered_map<std::string, std::vector<std::string>> enumDisplayNames;
    std::unordered_map<std::string, std::map<int, int64_t>> enumValueMaps;
    std::unordered_map<const ofAbstractParameter*, std::string> parameterPaths;
    std::unordered_set<std::string> selectorParameterPaths;

    // Support lookups for buildNaturalControls()/buildAdvancedControls():
    // a node's stable GenICam name (e.g. "ExposureTime") to its internal
    // path, and that path back to the already-constructed ofParameter living
    // in the raw tree, so curated tiers can alias real parameters instead of
    // re-walking the node map or duplicating state.
    std::unordered_map<std::string, std::string> genicamNameToPath;
    std::unordered_map<std::string, ofAbstractParameter*> pathToParameter;

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
    // Set alongside pendingRebuildRequested when the request happens while
    // restoring a saved configuration: the value that triggered the rebuild
    // was itself just set FROM that configuration, so the deferred rebuild
    // doesn't need to reapply it all over again.
    bool pendingRebuildSkipConfigReload = false;
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
