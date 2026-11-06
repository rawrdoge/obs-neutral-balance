cmake_minimum_required(VERSION 3.16...3.30)

# obs-neutral-balance - manual white balance correction filter for OBS Studio
# Derived from the official obs-plugintemplate (https://github.com/obsproject/obs-plugintemplate)
# Build against an installed OBS Studio (obs-studio dev package) or an OBS build tree.

project(obs-neutral-balance VERSION 2.0.1 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Testing
# ---------------------------------------------------------------------------
option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    
    # Add utest.h header
    include_directories(tests)
    
    # Create test executable
    add_executable(test_colorimetry
        tests/test_colorimetry.c
        src/colorimetry.c
        src/matrix.c
    )
    
    # Enable math constants
    if(WIN32)
        target_compile_definitions(test_colorimetry PRIVATE _USE_MATH_DEFINES)
    else()
        target_compile_definitions(test_colorimetry PRIVATE _GNU_SOURCE)
    endif()
    
    target_link_libraries(test_colorimetry PRIVATE m)
    
    # Add test to CTest
    add_test(NAME colorimetry_tests COMMAND test_colorimetry)
endif()

# ---------------------------------------------------------------------------
# Dependencies (only required for plugin build)
# ---------------------------------------------------------------------------
if(NOT BUILD_TESTING)
    find_package(libobs REQUIRED) # provides OBS::libobs
else()
    # For test-only builds, libobs is optional - try to find it for the plugin
    find_package(libobs QUIET)
endif()

# Only build the plugin if libobs is available (or if not test-only)
if(TARGET OBS::libobs OR NOT BUILD_TESTING)
    add_library(obs-neutral-balance MODULE)
    add_library(OBS::neutral-balance ALIAS obs-neutral-balance)
    
    target_sources(obs-neutral-balance PRIVATE
        src/plugin-main.c
        src/matrix.c
        src/colorimetry.c
        src/stats.c
        src/capture.c
        src/neutral-balance-filter.c
        src/eyedropper.cpp
    )
    
    target_include_directories(obs-neutral-balance PRIVATE src)
    
    if(TARGET OBS::libobs)
        target_link_libraries(obs-neutral-balance PRIVATE OBS::libobs)
    endif()
    
    # glibc hides PTHREAD_MUTEX_RECURSIVE etc. under strict ANSI C; obs-studio's
    # own build defines _GNU_SOURCE globally - mirror that for portability.
    # MSVC needs _USE_MATH_DEFINES for M_PI.
    if(WIN32)
        target_compile_definitions(obs-neutral-balance PRIVATE _USE_MATH_DEFINES)
    else()
        target_compile_definitions(obs-neutral-balance PRIVATE _GNU_SOURCE)
    endif()
    
    set_target_properties(obs-neutral-balance PROPERTIES
        PREFIX ""
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    
    if(MSVC)
        target_compile_options(obs-neutral-balance PRIVATE /W4)
    else()
        target_compile_options(obs-neutral-balance PRIVATE -Wall -Wextra)
    endif()
    
    # ---------------------------------------------------------------------------
    # Optional Qt6/Qt5 eyedropper dialog. Without Qt the filter still builds and
    # the automatic methods work; the "Pick Neutral" button is disabled.
    # ---------------------------------------------------------------------------
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(NOT TARGET Qt6::Widgets)
        find_package(Qt5 COMPONENTS Widgets QUIET)
    endif()
    
    if(TARGET Qt6::Widgets)
        target_link_libraries(obs-neutral-balance PRIVATE Qt6::Widgets)
        target_compile_definitions(obs-neutral-balance PRIVATE NB_HAVE_QT=1)
        set_target_properties(obs-neutral-balance PROPERTIES AUTOMOC ON)
    elseif(TARGET Qt5::Widgets)
        target_link_libraries(obs-neutral-balance PRIVATE Qt5::Widgets)
        target_compile_definitions(obs-neutral-balance PRIVATE NB_HAVE_QT=1)
        set_target_properties(obs-neutral-balance PROPERTIES AUTOMOC ON)
    else()
        message(WARNING "Qt Widgets not found - building without the eyedropper dialog")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install rules (mirror the layout OBS uses for bundled plugins)
# ---------------------------------------------------------------------------
if(WIN32)
    set(_plugin_dest obs-plugins/64bit)
    set(_data_dest data/obs-plugins/obs-neutral-balance)
elseif(APPLE)
    set(_plugin_dest obs-plugins)
    set(_data_dest data/obs-plugins/obs-neutral-balance)
else()
    set(_plugin_dest lib/obs-plugins)
    set(_data_dest share/obs/obs-plugins/obs-neutral-balance)
endif()

if(TARGET obs-neutral-balance)
    install(TARGETS obs-neutral-balance
        RUNTIME DESTINATION ${_plugin_dest}
        LIBRARY DESTINATION ${_plugin_dest}
    )
endif()

install(FILES data/neutral-balance.effect data/locale/en-US.ini
    DESTINATION ${_data_dest}
)