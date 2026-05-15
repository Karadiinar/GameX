#pragma once
#include <vulkan/vulkan.h>

enum class PresentModeSetting {
    Immediate = 0, // Uncapped (8000 FPS room-heater mode)
    VSync,         // Frame-capped, synchronized (FIFO)
    TripleBuffer   // Low-latency, frame-capped fallback (MAILBOX)
};

struct GraphicsConfig {
    // Present mode configuration
    PresentModeSetting presentMode = PresentModeSetting::TripleBuffer;
    
    // Window settings
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;
    
    // Future settings placeholders
    bool enableValidationLayers = true;
    bool shadowQuality = true;
};