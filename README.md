# Codec Foundation for Adobe CC

This is the community-supplied foundation for creating plugins to Adobe CC 2018 and 2019.

Development of this plugin was sponsored by
 - [disguise](http://disguise.one), makers of the disguise show production software and hardware.
 - [10bitFX](http://notch.one), creators of the Notch VFX software

Principal contributors to this plugin are

-  Greg Bakker (gbakker@gmail.com)
-  Richard Sykes
-  [Tom Butterworth](http://kriss.cx/tom)
-  [Nick Zinovenko](https://github.com/exscriber)

Thanks to Tom Butterworth for creating the Hap codec and Vidvox for supporting that development.

Please see [license.txt](license.txt) for the licenses of this plugin and the components used to create it.

# Development

The following information is for developers who wish to contribute to the project.

## Prerequisites

### compiler toolchain

You'll need a compiler environment appropriate to your operating system. The current plugin has been developed on

-  win64 with Microsoft Visual Studio 2017 Professional.
-  macOS with XCode

### CMake

CMake creates the build system for the supported target platforms. This project requires version 3.15.0 or later.

[https://cmake.org/install/](https://cmake.org/install/)

### NSIS

NSIS is required for win32 installer builds.

[http://nsis.sourceforge.net/Main_Page](http://nsis.sourceforge.net/Main_Page)

### Adobe CC 2019 SDKs

The following SDKs are required from Adobe.

<https://console.adobe.io/downloads>

| SDK           | Location                       |
|---------------|--------------------------------|
| Premiere      | external/adobe/premiere        |
| After Effects | external/adobe/AfterEffectsSDK |

### FFMpeg

FFmpeg 4.0 is used for output of the .mov format.

It is referenced as a submodule of this repository. Fetch the source for building with

    git submodule update --init

The FFMpeg build is not wrapped by the plugin's cmake build process, and must be made in a platform specific process as descibed below.

#### win64

Either install and set environment for your own FFMpeg, or build / install the one in external/ffmpeg as described at

<https://trac.ffmpeg.org/wiki/CompilationGuide/MSVC>

For reference, the FFMpeg build for the win64 plugin was created by

-  first installing [MSYS](http://www.mingw.org/wiki/msys)

-  launching a Visual Studio 2017 developer prompt
-  set Visual Studio build vars for an x64 build. Something like

        "C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvarsall.bat" amd64

-  running the MSYS shell from within that prompt

        C:\MinGW\msys\1.0\msys.bat
 
-  going to the external/ffmpeg/FFMPeg directory and then

        ./configure --toolchain=msvc --disable-x86asm --disable-network --disable-everything --enable-muxer=mov --enable-demuxer=mov --extra-cflags="-MD -arch:AVX"
        make

This will take a while.

#### macOS

Build a local FFmpeg by opening a terminal and moving to external/ffmpeg/FFmpeg. Then

    ./configure --disable-x86asm --disable-network --disable-everything --enable-muxer=mov --enable-demuxer=mov --disable-zlib --disable-iconv
    make

#### macOS

For macos, the homebrew installation is recommended (requires [Homebrew](https://brew.sh)).

    brew install pandoc

##  Building

### win64

First create a build directory at the top level, and move into it

    mkdir build
    cd build

Invoke cmake to create a Visual Studio .sln

    cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..

This should create HapEncoder.sln in the current directory. Open it in Visual Studio:

    HapEncoder.sln

The encoder plugin (.prm) is created by building all.
The installer executable is made by building the PACKAGE target, which is excluded from the regular build.

### macOS

First create a build directory at the top level, and move into it

    mkdir Release
    cd Release

Invoke cmake to create makefiles etc for a Release build

    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 ..

Then do a build with

    make -j

Alternatively to create an Xcode project

    cmake -DCMAKE_OSX_ARCHITECTURES=x86_64 -G Xcode ..

Then open the generated Xcode project.

This will create a plugin at

    Release/source/premiere_CC2018/HapEncoderPlugin.bundle

To create an installer (requires Apple Developer Program membership for signing)

    cpack

To create an unsigned installer for development testing (do not distribute unsigned installers)

    cpack -D CPACK_PRODUCTBUILD_IDENTITY_NAME=

The installer is created in the Release directory.

#### Notarizing macOS Installers

Installers for macOS **must** be notarized by Apple. If you skip this step, users will not be able to install your software. Because it requires prior setup it is not automated. Follow the instructions under "Upload Your App to the Notarization Service" in Apple's [Customizing the Notarization Workflow](https://developer.apple.com/documentation/xcode/notarizing_your_app_before_distribution/customizing_the_notarization_workflow?language=objc) guide.
