To create a multistreaming OBS plugin that shares encoders to save CPU/GPU power, here is a step-by-step implementation guide:

1. Set Up the Development Environment
   Install CLion and CMake. You will need CMake version 3.30.5 or higher for Windows/macOS, or 3.28.3 for Ubuntu. For Windows development in CLion, it is highly recommended to configure your toolchain to use the MSVC compiler.

2. Initialize with the Official Template
   Clone the official obsproject/obs-plugintemplate repository. This repository provides the required boilerplate C/C++ code, the necessary CMakeLists.txt build system, and pre-configured GitHub Actions for automated cross-platform building.

3. Define Module Entry Points
   In your core C/C++ source file, use the OBS_DECLARE_MODULE() macro. You must implement the obs_module_load() function to register your plugin's functionality upon startup, and the obs_module_unload() function to safely clean up memory when the plugin is removed or OBS closes.

4. Create the User Interface (Qt Dock)
   Your plugin needs a graphical interface for users to add target platforms and stream keys. Build a custom Qt widget that inherits from QDockWidget. To integrate this panel into OBS, retrieve the main application window by calling obs_frontend_get_main_window(), and attach your widget using QMainWindow::addDockWidget(). Avoid using the older obs_frontend_add_dock function, as it has been deprecated.

5. Implement Encoder Sharing Logic (The Core Engine)
   To prevent the plugin from overloading the user's system, you must clone the main stream's compression data instead of rendering video multiple times.

Instantiate new RTMP network outputs for your target platforms using obs_output_create().

Create and link streaming services (which hold the URLs and keys) to these outputs using obs_output_set_service().

Capture the active video and audio encoders from the main OBS stream using obs_output_get_video_encoder() and obs_output_get_audio_encoder().

Attach these existing encoders to your newly created secondary outputs using obs_output_set_video_encoder() and obs_output_set_audio_encoder().

Start the transmission to the secondary platforms using obs_output_start().

6. Handle Platform-Specific Requirements

Kick: Kick requires a secure connection. Ensure your service configuration string explicitly uses the RTMPS protocol and appends port 443 (e.g., rtmps://fa723fc1b171.global-contribute.live-video.net:443/app).

Instagram & TikTok: These platforms strictly enforce a vertical 9:16 aspect ratio, such as 720x1280. If a user wants to multistream to these platforms, you cannot use the shared horizontal encoder. Your plugin will need to provide an option to spawn an independent, dedicated video encoder pipeline specifically cropped and scaled for vertical output.

7. Compile and Release
   Once the implementation is complete, push your code to your GitHub repository. The obs-plugintemplate includes CI/CD pipelines that will automatically compile the code, check formatting, and generate the final installer artifacts for Windows, macOS, and Linux whenever you push a semantic version tag (like 1.0.0).