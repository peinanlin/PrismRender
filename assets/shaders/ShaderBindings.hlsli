#ifndef PRISM_SHADER_BINDINGS_HLSLI
#define PRISM_SHADER_BINDINGS_HLSLI

// D3D register classes share numbers, while Vulkan uses one binding namespace per set.
#define PRISM_VK_BINDING(bindingIndex) [[vk::binding(bindingIndex, 0)]]

#endif
