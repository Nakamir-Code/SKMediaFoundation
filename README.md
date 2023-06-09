# Media Foundation Samples using StereoKit
In an ideal world, MediaFoundation would simply explain how one can make use of their MFTs to send some data in and get some data back out. I hope these samples streamline your experience with Media Foundation, and soon enough, you'll have your own hardware-accelerated encoders/decoders running on a headset. These high-performance MFTs are mainly geared toward the low-resource HoloLens 2. These examples just show how to use the API, and may not work straight out of the box for UWP, but `mf_encoder.cpp` and `mf_decoder.cpp` always will.

## Getting Started
1. Run `build.ps1`
2. Choose one scenario from [main.cpp](src/main.cpp)
3. *Skip to #4 if you don't care about UWP*. You'll need to enable permissions in `package.appxManifest` for the scenario you want to run (e.g., `Internet (Client)` for [mf_decode_from_url.cpp](src/examples/mf_decode_from_url.cpp)). You may also need to `Rebuild` the project to copy the `Assets` folder for deployment.
4. Build the SKMediaFoundation project and deploy!