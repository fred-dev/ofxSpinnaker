#include "ofApp.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// ImGui's SliderBehavior() asserts that bounds stay within roughly
// +/-FLT_MAX/2 and aborts if they don't. Some GenICam float nodes report an
// effectively "unbounded" min/max (e.g. as DBL_MAX), which overflows to +/-inf
// when narrowed to float in ofxSpinnakerCamera::traverseNode() - sanitize
// before handing bounds to ImGui rather than trusting every node's range.
float sanitizeSliderBound(float value, float fallback) {
    constexpr float kImGuiSafeLimit = 3.40282347e+38f / 2.0f;
    if (!std::isfinite(value)) {
        return fallback;
    }
    return ofClamp(value, -kImGuiSafeLimit, kImGuiSafeLimit);
}

// ImGui's int SliderBehavior() checks a much tighter bound than the float
// one - roughly +/-INT32_MAX/2, not +/-FLT_MAX/2 - so it needs its own
// sanitizer rather than round-tripping through the float version above.
// Deliberately NOT using ofClamp() here: it's float-only (float value, float
// min, float max), and INT32_MAX/2 (1073741823) isn't exactly representable
// in a 32-bit float - it rounds up to 1073741824, one past the boundary
// ImGui actually asserts on. Plain integer comparison avoids that entirely.
int sanitizeIntSliderBound(int value) {
    constexpr int kImGuiSafeLimit = std::numeric_limits<int>::max() / 2;
    if (value > kImGuiSafeLimit) {
        return kImGuiSafeLimit;
    }
    if (value < -kImGuiSafeLimit) {
        return -kImGuiSafeLimit;
    }
    return value;
}

} // namespace

//--------------------------------------------------------------
void ofApp::setup(){
	ofSetLogLevel(OF_LOG_NOTICE);
	ofSetWindowTitle("ofxSpinnaker Example");
	ofBackground(20);

	gui.setup();

	// Cameras are discovered once here, on startup. Use the "Rescan Cameras"
	// button in the GUI to re-scan later (e.g. after plugging in a camera).
	spinnaker.setup(ofToDataPath("spinnaker_configs", true));
}

//--------------------------------------------------------------
void ofApp::update(){
	spinnaker.update();
}

//--------------------------------------------------------------
void ofApp::draw(){
	const size_t numCameras = spinnaker.getNumCameras();

	if (numCameras > 0) {
		const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(numCameras))));
		const int rows = static_cast<int>(std::ceil(numCameras / static_cast<double>(columns)));
		const float cellWidth = ofGetWidth() / static_cast<float>(columns);
		const float cellHeight = ofGetHeight() / static_cast<float>(rows);

		for (size_t i = 0; i < numCameras; ++i) {
			auto camera = spinnaker.getCamera(i);
			if (!camera) {
				continue;
			}
			const int col = static_cast<int>(i) % columns;
			const int row = static_cast<int>(i) / columns;
			drawCameraFitted(*camera, col * cellWidth, row * cellHeight, cellWidth, cellHeight);
		}
	} else {
		ofDrawBitmapStringHighlight("No Spinnaker cameras detected.", 20, 20);
	}

	gui.begin();

	ImGui::Begin("Cameras");
	ImGui::Text("%d camera(s) connected", static_cast<int>(numCameras));
	if (ImGui::Button("Rescan Cameras")) {
		// Discovery otherwise only happens on startup; this is the manual
		// escape hatch for hot-plugging a camera mid-session.
		spinnaker.refreshCameraList();
	}
	ImGui::End();

	for (size_t i = 0; i < numCameras; ++i) {
		auto camera = spinnaker.getCamera(i);
		if (!camera) {
			continue;
		}

		const std::string windowTitle = camera->getDeviceDisplayName() + " (" + camera->getSerialNumber() + ")";
		ImGui::SetNextWindowSize(ImVec2(380, 480), ImGuiCond_FirstUseEver);
		ImGui::Begin(windowTitle.c_str());

		ofParameterGroup& root = camera->getParameterRoot();

		// Camera Controls (ofxSpinnakerCamera::buildNaturalControls()) is the
		// primary surface: exposure, gain, white balance, saturation/tint if
		// the camera has them, and frame rate - always visible, open by
		// default.
		for (auto& sectionPtr : root) {
			if (sectionPtr->type() == typeid(ofParameterGroup).name() &&
				sectionPtr->castGroup().getName() == "Camera Controls") {
				ImGui::PushID(sectionPtr.get());
				if (ImGui::CollapsingHeader("Camera Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
					drawParameterGroup(*camera, sectionPtr->castGroup(), true);
				}
				ImGui::PopID();
				break;
			}
		}

		ImGui::Separator();
		// Advanced Controls (resolution via binning, a filtered pixel-format
		// picker) and the raw Info/Live Controls/Reconfigure Controls tree are
		// both hidden - not just collapsed - until explicitly opted into, so
		// a casual user adjusting exposure and gain never even sees them.
		ImGui::Checkbox("Show Advanced Settings", &showAdvanced);
		ImGui::Checkbox("Show Full Camera Settings", &showFull);

		for (auto& sectionPtr : root) {
			if (sectionPtr->type() != typeid(ofParameterGroup).name()) {
				continue;
			}

			ofParameterGroup& section = sectionPtr->castGroup();
			const std::string sectionName = section.getName();

			if (sectionName == "Camera Controls") {
				continue; // already drawn above
			}
			if (sectionName == "Advanced Controls") {
				if (!showAdvanced) {
					continue;
				}
			} else if (!showFull) {
				continue; // Info / Live Controls / Reconfigure Controls
			}

			ImGui::PushID(sectionPtr.get());
			if (ImGui::CollapsingHeader(sectionName.c_str())) {
				drawParameterGroup(*camera, section, false);
			}
			ImGui::PopID();
		}

		ImGui::End();
	}

	gui.end();
}

//--------------------------------------------------------------
void ofApp::drawCameraFitted(ofxSpinnakerCamera& camera, float cellX, float cellY, float cellW, float cellH){
	ofTexture& texture = camera.getTexture();
	if (!texture.isAllocated()) {
		return;
	}

	const float texW = texture.getWidth();
	const float texH = texture.getHeight();
	if (texW <= 0.0f || texH <= 0.0f) {
		return;
	}

	const float scale = std::min(cellW / texW, cellH / texH);
	const float drawW = texW * scale;
	const float drawH = texH * scale;
	const float drawX = cellX + (cellW - drawW) * 0.5f;
	const float drawY = cellY + (cellH - drawH) * 0.5f;

	texture.draw(drawX, drawY, drawW, drawH);
}

//--------------------------------------------------------------
void ofApp::drawParameterGroup(ofxSpinnakerCamera& camera, ofParameterGroup& group, bool defaultOpen){
	for (auto& paramPtr : group) {
		auto parameter = paramPtr;
		const std::string name = parameter->getName();

		ImGui::PushID(parameter.get());

		if (parameter->type() == typeid(ofParameterGroup).name()) {
			const ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
			if (ImGui::TreeNodeEx(name.c_str(), flags)) {
				drawParameterGroup(camera, parameter->castGroup(), defaultOpen);
				ImGui::TreePop();
			}
			ImGui::PopID();
			continue;
		}

		if (parameter->type() == typeid(ofParameter<std::string>).name()) {
			auto& stringParam = parameter->cast<std::string>();
			ImGui::TextWrapped("%s: %s", name.c_str(), stringParam.get().c_str());
			ImGui::PopID();
			continue;
		}

		// Live query: reflects Auto/Manual dependencies (e.g. ExposureAuto !=
		// Off disabling ExposureTime) and streaming-state dependencies the
		// instant they change, without waiting for a tree rebuild.
		const bool writable = camera.isParameterCurrentlyWritable(*parameter);
		ImGui::BeginDisabled(!writable);

		if (parameter->type() == typeid(ofParameter<int>).name()) {
			auto& intParam = parameter->cast<int>();
			const std::vector<std::string>* enumNames = camera.getEnumEntryNames(*parameter);

			if (enumNames) {
				int index = intParam.get();
				std::vector<const char*> items;
				items.reserve(enumNames->size());
				for (auto& entryName : *enumNames) {
					items.push_back(entryName.c_str());
				}
				if (ImGui::Combo(name.c_str(), &index, items.data(), static_cast<int>(items.size()))) {
					intParam.set(index);
				}
			} else {
				int value = intParam.get();
				const int minV = sanitizeIntSliderBound(intParam.getMin());
				int maxV = sanitizeIntSliderBound(intParam.getMax());
				if (maxV <= minV) {
					maxV = minV + 1;
				}
				if (ImGui::SliderInt(name.c_str(), &value, minV, maxV)) {
					intParam.set(value);
				}
			}
		} else if (parameter->type() == typeid(ofParameter<float>).name()) {
			auto& floatParam = parameter->cast<float>();
			float value = floatParam.get();
			const float minV = sanitizeSliderBound(floatParam.getMin(), 0.0f);
			float maxV = sanitizeSliderBound(floatParam.getMax(), minV + 1.0f);
			if (maxV <= minV) {
				maxV = minV + 1.0f;
			}
			if (ImGui::SliderFloat(name.c_str(), &value, minV, maxV)) {
				floatParam.set(value);
			}
		} else if (parameter->type() == typeid(ofParameter<bool>).name()) {
			auto& boolParam = parameter->cast<bool>();
			bool value = boolParam.get();
			if (ImGui::Checkbox(name.c_str(), &value)) {
				boolParam.set(value);
			}
		}

		ImGui::EndDisabled();
		ImGui::PopID();
	}
}

//--------------------------------------------------------------
void ofApp::exit(){
	spinnaker.shutdown();
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key){

}

//--------------------------------------------------------------
void ofApp::keyReleased(int key){

}

//--------------------------------------------------------------
void ofApp::mouseMoved(int x, int y ){

}

//--------------------------------------------------------------
void ofApp::mouseDragged(int x, int y, int button){

}

//--------------------------------------------------------------
void ofApp::mousePressed(int x, int y, int button){

}

//--------------------------------------------------------------
void ofApp::mouseReleased(int x, int y, int button){

}

//--------------------------------------------------------------
void ofApp::mouseScrolled(int x, int y, float scrollX, float scrollY){

}

//--------------------------------------------------------------
void ofApp::mouseEntered(int x, int y){

}

//--------------------------------------------------------------
void ofApp::mouseExited(int x, int y){

}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h){

}

//--------------------------------------------------------------
void ofApp::gotMessage(ofMessage msg){

}

//--------------------------------------------------------------
void ofApp::dragEvent(ofDragInfo dragInfo){

}
