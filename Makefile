MINGW64_CXX ?= x86_64-w64-mingw32-g++
MINGW64_CC  ?= x86_64-w64-mingw32-gcc
MINGW32_CXX ?= i686-w64-mingw32-g++
MINGW32_CC  ?= i686-w64-mingw32-gcc
WINDRES64   ?= x86_64-w64-mingw32-windres
WINDRES32   ?= i686-w64-mingw32-windres
WINE        ?= wine
PYTHON      ?= python3
BUILD_DIR   ?= build/linux-x64
DIST_DIR    := $(BUILD_DIR)/dist
TOOLCHAIN   := $(BUILD_DIR)/mingw-toolchain.cmake
LAUNCHER_BUILD := build/linux-x86
NPROC       := $(shell nproc 2>/dev/null || echo 4)

LOCALE_DEF_H := thprac/src/thprac/thprac_locale_def.h
LOCALE_DEF_C := thprac/src/thprac/thprac_locale_def.cpp
LOC_JSON_SRC := thprac/loc_json.cpp thprac/src/3rdParties/yyjson/yyjson.c
LOC_JSON_EXE := $(BUILD_DIR)/loc_json.exe
STAMP_NC     := $(BUILD_DIR)/.built
BRIDGE_RES   := $(BUILD_DIR)/bridge.res

BIN_DLL    := $(BUILD_DIR)/bin/libthprac_th06nc.dll
BIN_FT     := $(BUILD_DIR)/bin/freetype.dll
BIN_BRIDGE := $(BUILD_DIR)/bin/thprac_bridge64.exe

INTEGRATED_EXE := $(DIST_DIR)/thprac-th06nc.exe

.PHONY: all clean
.DEFAULT_GOAL := all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LOC_JSON_EXE): $(LOC_JSON_SRC) | $(BUILD_DIR)
	$(MINGW64_CXX) -Ithprac/src/3rdParties/yyjson -O2 -std=c++20 -municode $(LOC_JSON_SRC) -o $@ -static

$(BUILD_DIR)/.locale: $(LOC_JSON_EXE) thprac/src/thprac/thprac_games_def.json
	$(WINE) $(LOC_JSON_EXE) thprac/src/thprac
	touch $@

$(LOCALE_DEF_C) $(LOCALE_DEF_H): $(BUILD_DIR)/.locale
	@:

$(TOOLCHAIN): | $(BUILD_DIR)
	@printf '%s\n' \
	'set(CMAKE_SYSTEM_NAME Windows)' \
	'set(CMAKE_SYSTEM_PROCESSOR x86_64)' \
	'set(CMAKE_C_COMPILER $(MINGW64_CC))' \
	'set(CMAKE_CXX_COMPILER $(MINGW64_CXX))' \
	'set(CMAKE_RC_COMPILER $(WINDRES64))' \
	'set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)' \
	'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' \
	'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' \
	'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' > $@

$(STAMP_NC): $(TOOLCHAIN) $(BUILD_DIR)/.locale CMakeLists.txt
	cmake -S . -B $(BUILD_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --target thprac_th06nc thprac_bridge64 -j$(NPROC)
	touch $@

$(BIN_DLL) $(BIN_FT) $(BIN_BRIDGE): $(STAMP_NC)
	@:

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(BRIDGE_RES): thprac/src/thprac/th06nc/bridge.rc thprac/src/thprac/th06nc/bridge_resource.h thprac/src/thprac/th06nc/bridge.manifest $(BIN_DLL) $(BIN_FT) | $(BUILD_DIR)
	sed -e 's|FREETYPE_DLL_PATH|$(abspath $(BIN_FT))|g' -e 's|MODULE_DLL_PATH|$(abspath $(BIN_DLL))|g' thprac/src/thprac/th06nc/bridge.rc > $(BUILD_DIR)/bridge_gen.rc
	$(WINDRES64) -Ithprac/src/thprac/th06nc $(BUILD_DIR)/bridge_gen.rc -O coff -o $@

# --- Win32 launcher (integrated) ---
LAUNCHER_SRCS := $(shell cat tools/launcher_srcs.txt)
LAUNCHER_INCLUDES := -Ithprac/src -Ithprac/src/thprac -Ithprac/src/3rdParties -Ithprac/src/3rdParties/ImGui -Ithprac/src/3rdParties/FreeType/include -Ithprac/src/3rdParties/yyjson -Ithprac/src/3rdParties/MetroHash -Ithprac/src/3rdParties/MinHook/include -Ithprac/src/3rdParties/d3d8/include
LAUNCHER_DEFS := -DNOMINMAX -DUNICODE -D_UNICODE -DHAVE_SSE2 -D_CRT_SECURE_NO_WARNINGS -D_WINDOWS -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DDISTORM_LIGHT
LAUNCHER_CXXFLAGS := -std=c++20 -municode -O2 -msse -msse2 -mfpmath=sse -finput-charset=UTF-8 -fexec-charset=UTF-8 -ffunction-sections -fdata-sections -Wno-unknown-pragmas -Wno-attributes -fms-extensions -fpermissive $(LAUNCHER_DEFS) $(LAUNCHER_INCLUDES)
LAUNCHER_CFLAGS := -O2 -msse -msse2 -mfpmath=sse -finput-charset=UTF-8 -fexec-charset=UTF-8 -ffunction-sections -fdata-sections -Wno-unknown-pragmas $(LAUNCHER_DEFS) $(LAUNCHER_INCLUDES)
LAUNCHER_LDFLAGS = -static -municode -Wl,--subsystem,windows -Wl,--gc-sections -static-libgcc -static-libstdc++ -L$(FT32_PREFIX)/lib -lfreetype -ld3d9 -lgdi32 -luser32 -lole32 -lcomdlg32 -lshell32 -ladvapi32 -lntdll -lpsapi -lversion -limm32 -lwinmm -ldbghelp -luuid -lsetupapi -Wl,-Bstatic -l:libwinpthread.a
OBJDUMP32 ?= i686-w64-mingw32-objdump

$(LAUNCHER_BUILD):
	mkdir -p $(LAUNCHER_BUILD)

# --- 32-bit FreeType (static) for the Win32 launcher, built from source into build/ ---
FT_VERSION  ?= 2.13.3
FT_URL      ?= https://download.savannah.gnu.org/releases/freetype/freetype-$(FT_VERSION).tar.xz
FT32_DIR    := $(LAUNCHER_BUILD)/freetype
FT32_PREFIX := $(abspath $(FT32_DIR)/install)
FT32_LIB    := $(FT32_PREFIX)/lib/libfreetype.a
TOOLCHAIN32 := $(LAUNCHER_BUILD)/mingw32-toolchain.cmake

$(TOOLCHAIN32): | $(LAUNCHER_BUILD)
	@printf '%s\n' \
	'set(CMAKE_SYSTEM_NAME Windows)' \
	'set(CMAKE_SYSTEM_PROCESSOR i686)' \
	'set(CMAKE_C_COMPILER $(MINGW32_CC))' \
	'set(CMAKE_CXX_COMPILER $(MINGW32_CXX))' \
	'set(CMAKE_RC_COMPILER $(WINDRES32))' \
	'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' \
	'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' \
	'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' > $@

$(FT32_LIB): $(TOOLCHAIN32) | $(LAUNCHER_BUILD)
	mkdir -p $(FT32_DIR)
	test -f $(FT32_DIR)/freetype-$(FT_VERSION).tar.xz || curl -fL -o $(FT32_DIR)/freetype-$(FT_VERSION).tar.xz $(FT_URL)
	tar -xf $(FT32_DIR)/freetype-$(FT_VERSION).tar.xz -C $(FT32_DIR)
	cmake -S $(FT32_DIR)/freetype-$(FT_VERSION) -B $(FT32_DIR)/build \
		-DCMAKE_TOOLCHAIN_FILE=$(abspath $(TOOLCHAIN32)) -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=$(FT32_PREFIX) \
		-DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
	cmake --build $(FT32_DIR)/build -j$(NPROC)
	cmake --install $(FT32_DIR)/build

all: $(INTEGRATED_EXE)

$(INTEGRATED_EXE): $(BUILD_DIR)/.locale thprac/thprac.rc tools/launcher_srcs.txt tools/make_payload_rc.py $(FT32_LIB) $(BIN_DLL) $(BIN_FT) $(BIN_BRIDGE) README_NC.md LICENCE THIRD_PARTY_NOTICES.txt | $(LAUNCHER_BUILD) $(DIST_DIR)
	mkdir -p $(LAUNCHER_BUILD)/payload/licenses $(LAUNCHER_BUILD)/bundle
	cp $(BIN_DLL) $(LAUNCHER_BUILD)/payload/thprac_th06nc.dll
	cp $(BIN_FT) $(LAUNCHER_BUILD)/payload/freetype.dll
	cp $(BIN_BRIDGE) $(LAUNCHER_BUILD)/payload/thprac_bridge64.exe
	cp README_NC.md LICENCE THIRD_PARTY_NOTICES.txt $(LAUNCHER_BUILD)/payload/
	cp thprac/src/3rdParties/FreeType/LICENSE.TXT $(LAUNCHER_BUILD)/payload/licenses/FreeType-LICENSE.txt
	cp thprac/src/3rdParties/FreeType/FTL.TXT $(LAUNCHER_BUILD)/payload/licenses/FreeType-FTL.txt
	cp thprac/src/3rdParties/MinHook/LICENSE.txt $(LAUNCHER_BUILD)/payload/licenses/MinHook.txt
	cp thprac/src/3rdParties/ImGui/LICENSE.txt $(LAUNCHER_BUILD)/payload/licenses/ImGui.txt
	$(PYTHON) tools/make_payload_rc.py $(LAUNCHER_BUILD)/payload $(LAUNCHER_BUILD)/bundle/payload.rc $(LAUNCHER_BUILD)/bundle
	$(WINDRES32) -Ithprac -Ithprac/src/thprac thprac/thprac.rc -O coff -o $(LAUNCHER_BUILD)/thprac.rc.o
	$(WINDRES32) $(LAUNCHER_BUILD)/bundle/payload.rc -O coff -o $(LAUNCHER_BUILD)/bundle/payload.res
	$(MINGW32_CXX) $(LAUNCHER_CXXFLAGS) $(LAUNCHER_SRCS) $(LAUNCHER_BUILD)/thprac.rc.o $(LAUNCHER_BUILD)/bundle/payload.res -o $@ $(LAUNCHER_LDFLAGS)
	@if $(OBJDUMP32) -p $@ | grep -qiE 'DLL Name: (libstdc\+\+|libwinpthread|libgcc_s)'; then echo "error: $@ still imports MinGW runtime DLLs (not fully static)"; rm -f $@; exit 1; fi

clean:
	rm -rf $(BUILD_DIR) $(LAUNCHER_BUILD)
