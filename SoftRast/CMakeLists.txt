cmake_minimum_required(VERSION 3.2)

set(SOFT_RAST_FILES
    "Config.h"
    "Rasterizer.cpp"
    "Rasterizer.h"
	"SoftRastTypes.h"
	"Texture.h"
	"Texture.cpp"
	"stb_image.h"
    "stb_image.cpp"
    "stb_image_resize.h"
    "stb_image_resize.cpp"
    "Renderer.h"
    "Renderer.cpp"
    "Binning.h"
    "Binning.cpp"
    "TaskSystem.h"
    "TaskSystem.cpp"
    "SIMDUtil.h"
)

add_library(SoftRast ${SOFT_RAST_FILES})
target_link_libraries(SoftRast kt microprofile)
target_include_directories(SoftRast PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})