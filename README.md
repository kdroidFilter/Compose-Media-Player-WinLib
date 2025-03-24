# NativeVideoPlayer

NativeVideoPlayer is a native Windows library that forms part of the [ComposeMediaPlayer](https://github.com/kdroidFilter/ComposeMediaPlayer) project. It provides media playback functionality using Microsoft's Media Foundation framework. This library is designed for integration with Compose-based applications to deliver smooth video and audio playback on Windows.

## Features

- **Media Playback:** Play media files and streams (via file paths or URLs) using the Media Foundation API.
- **Audio Control:** Adjust volume, mute/unmute, and retrieve per-channel audio levels.
- **Video Display:** Retrieve video dimensions, update the display, and manage aspect ratios.
- **Thread-safe:** Utilizes critical sections to ensure thread safety during playback operations.
- **Callback Notifications:** Provides event notifications (media created, playback started/paused/stopped, errors, and more) through user-defined callbacks.

## Getting Started

### Prerequisites

- **Windows 10 or later:** The library targets Windows 10 (`WINVER=_WIN32_WINNT_WIN10`).
- **C++20 Compiler:** Ensure your development environment supports C++20.
- **Media Foundation Libraries:** The library links against Media Foundation libraries (e.g., `mfplay`, `mf`, `mfplat`, etc.).

### Building the Library

The project uses CMake for its build system. To build the library:

1. **Clone the Repository:**
   ```bash
   git clone https://github.com/kdroidFilter/ComposeMediaPlayer.git
   cd ComposeMediaPlayer
   ```

2. **Create a Build Directory:**
   ```bash
   mkdir build && cd build
   ```

3. **Generate Build Files:**
   ```bash
   cmake ..
   ```

4. **Build the Project:**
   ```bash
   cmake --build .
   ```

> **Note:** This project is compiled using the **minsizedel** profile, which is configured to optimize the build size.

This will generate the `NativeVideoPlayer.dll` along with the associated header files.

### Integration

To integrate the library into your project:

1. Include the header file in your project:
   ```cpp
   #include "NativeVideoPlayer.h"
   ```

2. Link against the generated `NativeVideoPlayer.dll` and ensure the DLL is available in your executable's path.

3. Initialize the media player by calling:
   ```cpp
   HRESULT hr = InitializeMediaPlayer(hwnd, MyMediaPlayerCallback);
   ```
   Replace `hwnd` with your window handle and `MyMediaPlayerCallback` with your callback function to handle player events.

## API Overview

The library exposes several functions:

- **Initialization & Cleanup:**
  - `InitializeMediaPlayer(HWND hwnd, MEDIA_PLAYER_CALLBACK callback)`
  - `CleanupMediaPlayer()`
  - `IsInitialized()`

- **Playback Control:**
  - `PlayFile(const wchar_t* filePath)`
  - `PlayURL(const wchar_t* url)`
  - `PausePlayback()`
  - `ResumePlayback()`
  - `StopPlayback()`
  - `IsPlaying()`
  - `IsLoading()`
  
- **Video & Audio:**
  - `UpdateVideo()`
  - `GetVideoSize(VideoSize* pSize)`
  - `GetVideoAspectRatio(float* pRatio)`
  - `SetVolume(float level)`
  - `GetVolume(float* pLevel)`
  - `SetMute(BOOL bMute)`
  - `GetMute(BOOL* pbMute)`
  - `GetChannelLevels(float* pLeft, float* pRight)`

- **Slider Functions:**
  - `GetDuration(LONGLONG* pDuration)`
  - `GetCurrentPosition(LONGLONG* pPosition)`
  - `SetPosition(LONGLONG position)`

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Contributing

Contributions to the ComposeMediaPlayer project are welcome. If you wish to contribute to NativeVideoPlayer or other parts of the project, please submit a pull request on GitHub.
