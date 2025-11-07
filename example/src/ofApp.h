#pragma once

#include "ofMain.h"
#include "ofxGui.h"
#include "ofxSpinnaker.h"
#include <deque>

class ofApp : public ofBaseApp {
public:
    void setup() override;
    void update() override;
    void draw() override;
    void exit() override;
    void keyPressed(int key) override;

private:
    struct CameraGui {
        std::shared_ptr<ofxSpinnakerCamera> camera;
        ofxPanel panel;
        uint64_t parameterRevision = 0;
    };

    void rebuildGui();

    ofxSpinnaker spinnaker;
    std::deque<CameraGui> cameraPanels;

    float guiWidth = 320.0f;
};
