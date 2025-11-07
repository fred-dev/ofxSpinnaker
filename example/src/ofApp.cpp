#include "ofApp.h"

namespace {
constexpr float kCameraPreviewHeight = 240.0f;
}

void ofApp::setup() {
    ofSetWindowTitle("ofxSpinnaker Multi-Camera");
    ofSetLogLevel(OF_LOG_NOTICE);

    spinnaker.setConfigurationDirectory(ofToDataPath("spinnaker_configs", true));

    if (!spinnaker.setup()) {
        ofLogError("ofApp") << "Failed to initialize ofxSpinnaker.";
    }

    rebuildGui();
}

void ofApp::update() {
    spinnaker.update();

    bool rebuildNeeded = cameraPanels.size() != spinnaker.getNumCameras();

    if (!rebuildNeeded) {
        for (auto& entry : cameraPanels) {
            if (!entry.camera) {
                rebuildNeeded = true;
                break;
            }

            if (entry.camera->consumeGuiRefreshFlag()) {
                rebuildNeeded = true;
                break;
            }

            const uint64_t revision = entry.camera->getParameterRevision();
            if (revision != entry.parameterRevision) {
                rebuildNeeded = true;
                break;
            }
        }
    }

    if (rebuildNeeded) {
        rebuildGui();
    }
}

void ofApp::draw() {
    ofBackground(20);

    float x = guiWidth + 40.0f;
    float y = 20.0f;
    const float spacing = 20.0f;

    for (size_t i = 0; i < spinnaker.getNumCameras(); ++i) {
        auto camera = spinnaker.getCamera(i);
        if (!camera) {
            continue;
        }

        const float aspect = camera->getTexture().isAllocated()
                                  ? camera->getTexture().getHeight() > 0
                                        ? camera->getTexture().getWidth() / camera->getTexture().getHeight()
                                        : 1.33f
                                  : 1.33f;

        const float previewWidth = kCameraPreviewHeight * aspect;

        ofSetColor(255);
        camera->draw(x, y, previewWidth, kCameraPreviewHeight);

        ofSetColor(255);
        ofDrawBitmapStringHighlight(camera->getDeviceDisplayName() + " (" + camera->getSerialNumber() + ")",
                                    x,
                                    y - 6.0f);

        y += kCameraPreviewHeight + spacing;
    }

    for (auto& panel : cameraPanels) {
        panel.panel.draw();
    }
}

void ofApp::exit() {
    cameraPanels.clear();
    spinnaker.shutdown();
}

void ofApp::keyPressed(int key) {
    if (key == 'r') {
        spinnaker.refreshCameraList();
        rebuildGui();
    }
}

void ofApp::rebuildGui() {
    cameraPanels.clear();

    float x = 20.0f;
    float y = 20.0f;
    const float spacingY = 10.0f;

    const auto numCameras = spinnaker.getNumCameras();
    for (size_t i = 0; i < numCameras; ++i) {
        auto camera = spinnaker.getCamera(i);
        if (!camera) {
            continue;
        }

        auto& entry = cameraPanels.emplace_back();
        entry.camera = std::move(camera);
        entry.panel.setup(entry.camera->getParameterRoot(), entry.camera->getDeviceDisplayName(), guiWidth, 14);
        entry.panel.setPosition(x, y);
        entry.parameterRevision = entry.camera->getParameterRevision();

        y += entry.panel.getHeight() + spacingY;
    }
}
