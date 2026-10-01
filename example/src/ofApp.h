#pragma once

#include "ofMain.h"
#include "ofxImGui.h"
#include "ofxSpinnaker.h"

class ofApp : public ofBaseApp{

	public:
		void setup() override;
		void update() override;
		void draw() override;
		void exit() override;

		void keyPressed(int key) override;
		void keyReleased(int key) override;
		void mouseMoved(int x, int y ) override;
		void mouseDragged(int x, int y, int button) override;
		void mousePressed(int x, int y, int button) override;
		void mouseReleased(int x, int y, int button) override;
		void mouseScrolled(int x, int y, float scrollX, float scrollY) override;
		void mouseEntered(int x, int y) override;
		void mouseExited(int x, int y) override;
		void windowResized(int w, int h) override;
		void dragEvent(ofDragInfo dragInfo) override;
		void gotMessage(ofMessage msg) override;

	private:
		// Recursively renders an ofParameterGroup (as built by ofxSpinnakerCamera)
		// into the current ImGui window. Because ImGui is immediate-mode, this
		// just walks the live tree fresh every frame - no explicit rebuild step
		// is needed on the GUI side even when the addon restructures the tree.
		// defaultOpen is threaded down from the top-level section (Info/Live
		// Controls/Reconfigure Controls) so every nested category within it
		// shares the same initial open/closed state.
		void drawParameterGroup(ofxSpinnakerCamera& camera, ofParameterGroup& group, bool defaultOpen);

		// Draws the camera's live texture centered and aspect-fit within the
		// given cell, rather than stretched to fill it.
		void drawCameraFitted(ofxSpinnakerCamera& camera, float cellX, float cellY, float cellW, float cellH);

		ofxSpinnaker spinnaker;
		ofxImGui::Gui gui;

		// Applies to every camera window uniformly (simple session-local UI
		// state, not persisted) - Camera Controls is always shown; these two
		// gate the more specialized/riskier tiers behind an explicit opt-in
		// rather than just a collapsed-but-visible header, so a casual user
		// doesn't stumble into resolution/pixel-format controls by accident.
		bool showAdvanced = false;
		bool showFull = false;
};
