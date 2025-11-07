# ofxSpinnaker

FLIR / Teledyne Spinnaker SDK integration for openFrameworks. The addon wraps
the Spinnaker C++ API through `ofxSpinnakerCamera`, providing easy access to
device discovery, capture, and parameter GUIs.

Because the Spinnaker SDK is released under its own license, **no binary SDK
headers or libraries are shipped in this repository**. Before building or using
`ofxSpinnaker`, you need to install the official SDK and copy the platform
artifacts into the addon.

## Prerequisites

- openFrameworks 0.12+ (tested with macOS makefiles)
- macOS 11 or newer on an Intel (x86_64) host
- Teledyne Spinnaker SDK 3.x for macOS (x86_64 download)

By default the Spinnaker installer places the SDK at `/Applications/Spinnaker`.
If you chose a different location, adjust the paths below accordingly.

## Syncing the Spinnaker SDK into the addon

1. Download and install the Spinnaker SDK that matches your platform and
   architecture (e.g. the "macOS x86_64" build for Intel Macs).
2. From the addon root, run the sync script to copy the headers and dylibs:

   ```sh
   cd addons/ofxSpinnaker
   ./scripts/sync_spinnaker_sdk.sh                # uses /Applications/Spinnaker
   # or specify an explicit install path
   ./scripts/sync_spinnaker_sdk.sh /path/to/Spinnaker
   ```

   The script performs the following:

   - Mirrors `include/` into `libs/spinnaker/include/`
   - Copies `.dylib` artifacts into `libs/spinnaker/lib/osx/`
   - Copies the GenTL runtime into `libs/spinnaker/lib/osx/flir-gentl/`

   The sync uses relative paths and assumes the SDK is under
   `/Applications/Spinnaker`, matching Teledyne's installer defaults.

3. Confirm the copied libraries match your host architecture if needed:

   ```sh
   lipo -info libs/spinnaker/lib/osx/libSpinnaker.dylib
   ```

## Building and running the example

Once the SDK assets are in place:

```sh
cd addons/ofxSpinnaker/example
make -j
make RunRelease   # launches the example app
```

The example automatically discovers connected Spinnaker cameras, exposes their
parameter trees via `ofxGui`, and streams preview textures.

## Using the addon in your project

1. Add `ofxSpinnaker` to your project's `addons.make`.
2. Ensure you sync the SDK into the addon before building (see steps above).
3. Optionally copy the sync script into your own workflow to refresh the SDK
   after updates.

Refer to the `src/ofxSpinnaker.h` header and the example project for API usage
patterns. The addon supports multi-camera capture, texture sharing, and runtime
parameter updates.


