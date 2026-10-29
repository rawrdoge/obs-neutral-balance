cmake_minimum_required(VERSION 3.16...3.30)

# obs-neutral-balance - manual white balance correction filter for OBS Studio
# Derived from the official obs-plugintemplate (https://github.com/obsproject/obs-plugintemplate)
# Build against an installed OBS Studio (obs-studio dev package) or an OBS build tree.

project(obs-neutral-balance VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(libobs REQUIRED) # provides OBS::libobs

add_library(obs-neutral-balance MODULE)
add_library(OBS::neutral-balance ALIAS obs-neutral-balance)

target_sources(obs-neutral-balance PRIVATE
	src/plugin-main.c
	src/neutral-balance-filter.c
	src/eyedropper.cpp
)

target_include_directories(obs-neutral-balance PRIVATE src)
target_link_libraries(obs-neutral-balance PRIVATE OBS::libobs)

# glibc hides PTHREAD_MUTEX_RECURSIVE etc. under strict ANSI C; obs-studio's
# own build defines _GNU_SOURCE globally - mirror that for portability.
if(NOT WIN32)
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

install(TARGETS obs-neutral-balance
	RUNTIME DESTINATION ${_plugin_dest}
	LIBRARY DESTINATION ${_plugin_dest}
)
install(FILES data/neutral-balance.effect data/locale/en-US.ini
	DESTINATION ${_data_dest}
)
