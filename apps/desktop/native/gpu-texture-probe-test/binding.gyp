{
  "targets": [
    {
      "target_name": "gpu_texture_probe_lifecycle_test",
      "sources": ["../gpu-texture-probe/src/addon.cpp"],
      "include_dirs": ["<!@(node -p \"require('node-addon-api').include\")"],
      "dependencies": ["<!(node -p \"require('node-addon-api').gyp\")"],
      "defines": [
        "NAPI_CPP_EXCEPTIONS",
        "NAPI_VERSION=8",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "_WIN32_WINNT=0x0A00",
        "GPU_TEXTURE_PROBE_TESTING"
      ],
      "libraries": ["d3d11.lib", "dxgi.lib", "ole32.lib"],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": ["/std:c++17"]
        }
      }
    }
  ]
}
