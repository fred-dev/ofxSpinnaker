meta:
    ADDON_NAME = ofxSpinnaker
    ADDON_DESCRIPTION = FLIR / Teledyne Spinnaker SDK integration for openFrameworks
    ADDON_AUTHOR = ofxSpinnaker maintainers
    ADDON_TAGS = "camera" "spinnaker" "flir" "machine-vision"
    ADDON_URL = https://github.com/yournamehere/ofxSpinnaker

common:
    ADDON_SOURCES += src/ofxSpinnaker.cpp
    ADDON_INCLUDES += $(ADDON_PATH)src
    ADDON_INCLUDES += $(ADDON_PATH)libs/spinnaker/include
    ADDON_INCLUDES += $(ADDON_PATH)libs/spinnaker/include/SpinGenApi
    ADDON_INCLUDES += $(ADDON_PATH)libs/spinnaker/include/spinc
    ADDON_INCLUDES += $(ADDON_PATH)libs/spinnaker/include/Interface

    # Silence deprecated OpenGL warnings on macOS builds
    ADDON_CPPFLAGS += -DGL_SILENCE_DEPRECATION

osx:
    SPINNAKER_LIB_DIR = $(ADDON_PATH)libs/spinnaker/lib/osx

    ADDON_LDFLAGS += -L$(SPINNAKER_LIB_DIR)
    ADDON_LDFLAGS += -Wl,-rpath,@executable_path/
    ADDON_LDFLAGS += -Wl,-rpath,@loader_path/
    # OF's macOS make build always produces an app bundle
    # (bin/<project>.app/Contents/MacOS/<project>); the dylibs land in bin/
    # itself (see note below) - three directories above the actual executable -
    # so the rpath needs to walk back out of the bundle to find them.
    ADDON_LDFLAGS += -Wl,-rpath,@executable_path/../../../
    ADDON_LDFLAGS += -Wl,-rpath,@loader_path/../../../

    # No dependency on /Applications/Spinnaker, /usr/local, or Homebrew being
    # present on the machine running the built app: scripts/sync_spinnaker_sdk.sh
    # rewrites every synced dylib's own ID and its absolute Homebrew dependency
    # paths (libomp, libusb) to @rpath, and bundles those two libraries in from
    # the SDK itself.

    # NOTE ON WHAT ACTUALLY CONTROLS LINKING/BUNDLING ON THIS PLATFORM:
    # openFrameworks' build system auto-discovers, links, AND copies to bin/
    # every .dylib it finds under an addon's libs/*/lib/<platform>/ folder
    # (config.addons.mk's parse_addons_libraries + compile.project.mk's
    # copyaddonslibs) - ADDON_LIBS entries only ever get appended to that
    # auto-discovered list, they can't remove anything from it, and there is no
    # ADDON_COPY_TO_BIN directive in this build system (an earlier version of
    # this file had one; it was silently inert). So the real control over what
    # gets linked and bundled is scripts/sync_spinnaker_sdk.sh's rsync
    # allowlist, not the ADDON_LIBS list below - these entries exist mainly as
    # documentation of the addon's actual dependency closure (Spinnaker's own
    # runtime plus the GenICam support libraries), deliberately excluding
    # SpinVideo/SpinVideo_C (unused, pulls in a full ffmpeg chain) and the
    # bundled CppUnit/GCBaseTest test-only libraries.
    ADDON_LIBS += Spinnaker
    ADDON_LIBS += Spinnaker_C
    ADDON_LIBS += SpinUpdate
    ADDON_LIBS += GenApi
    ADDON_LIBS += GCBase
    ADDON_LIBS += NodeMapData
    ADDON_LIBS += MathParser
    ADDON_LIBS += XmlParser
    ADDON_LIBS += log4cpp
    ADDON_LIBS += Log

    # Spinnaker's System::GetInstance() loads its GenTL producer (.cti) itself -
    # it's not just an optional interop path - so the driver bundle has to ship
    # too. ADDON_DATA (unlike ADDON_COPY_TO_BIN) is a real directive: it copies
    # into bin/data/, where ofxSpinnaker::setup() points SPINNAKER_GENTL64_CTI
    # before calling System::GetInstance().
    ADDON_DATA += libs/spinnaker/lib/osx/flir-gentl

vs:
    SPINNAKER_LIB_DIR = $(ADDON_PATH)libs/spinnaker/lib/vs

    ADDON_LDFLAGS += /LIBPATH:"$(SPINNAKER_LIB_DIR)"

    ADDON_LIBS += Spinnaker.lib
    ADDON_LIBS += Spinnaker_C.lib
    ADDON_LIBS += SpinUpdate.lib
    ADDON_LIBS += GenApi_MD.lib
    ADDON_LIBS += GCBase_MD.lib
    ADDON_LIBS += NodeMapData_MD.lib
    ADDON_LIBS += MathParser_MD.lib
    ADDON_LIBS += XmlParser_MD.lib
    ADDON_LIBS += log4cpp_MD.lib
    ADDON_LIBS += Log_MD.lib

    ADDON_DLL_COPY += $(SPINNAKER_LIB_DIR)/*.dll
    ADDON_DLL_COPY += $(SPINNAKER_LIB_DIR)/flir-gentl

linux64:
    ADDON_WARN = "Spinnaker Linux build support pending update"

linuxarmv6l:
    ADDON_WARN = "Spinnaker not supported on linuxarmv6l"

linuxarmv7l:
    ADDON_WARN = "Spinnaker not supported on linuxarmv7l"

android/armeabi:
    ADDON_WARN = "Spinnaker not available for Android"

android/armeabi-v7a:
    ADDON_WARN = "Spinnaker not available for Android"

ios:
    ADDON_WARN = "Spinnaker not available for iOS"

tvos:
    ADDON_WARN = "Spinnaker not available for tvOS"
