#!/usr/bin/env bash
# Generates a Visual Studio NMake (Makefile-type) project that wraps the
# existing MSYS2/MinGW make build for the ioEF SP engine.
# Re-run this after adding/removing source files to refresh the file lists.
set -euo pipefail
cd "$(dirname "$0")"

# MSYS-style path of this repo (e.g. /c/DEV/GitHub/Public/ioef-sp), baked into the
# generated NMake build commands. Derived from the script's own location so the
# project works on any machine/checkout; override with REPO_MSYS=... if needed.
REPO_MSYS="${REPO_MSYS:-$(pwd)}"

PROJ_GUID="{A1B2C3D4-E5F6-4789-ABCD-0123456789AB}"
SLN_GUID="{B2C3D4E5-F6A7-489B-BCDE-123456789ABC}"
NAME="ioef-sp"

# Engine/game directories worth browsing (skip bundled 3rd-party libs:
# jpeg-8c, libcurl*, libogg*, libspeex, libvorbis*, opus*, opusfile*, zlib, AL, asm, tools, SDL2).
DIRS="botlib cgame client game null q3_ui qcommon renderercommon renderergl1 renderergl2 sdl server sys ui vr"

# --- Determine which sources the Windows make build ACTUALLY compiles. ---
# Each compiled object has a .d dependency file whose first "code/*.c" token is
# the translation unit. Files not in this set (Android/Unix/VR/GL2/null stubs,
# etc.) are listed as <None> so they're browsable but IntelliSense ignores them
# (otherwise VS errors on missing platform headers like jni.h).
declare -A COMPILED COMPILED_DIR
ddir=""
for cand in build/release-mingw64-x86_64 build/release-mingw32-x86 build/debug-mingw64-x86_64 build/debug-mingw32-x86; do
  if [ -d "$cand" ] && find "$cand" -name '*.d' -print -quit | grep -q .; then ddir="$cand"; break; fi
done
if [ -n "$ddir" ]; then
  while read -r src; do
    [ -n "$src" ] || continue
    COMPILED["$src"]=1
    COMPILED_DIR["$(dirname "$src")"]=1
  done < <(find "$ddir" -name '*.d' -exec sh -c 'grep -oE "code/[A-Za-z0-9_./-]+\.c" "$1" | head -1' _ {} \; | sort -u)
  echo "Using compiled set from $ddir (${#COMPILED[@]} sources)."
else
  echo "WARNING: no build/*/*.d files found; run a build first for accurate IntelliSense classification. Treating all sources as compiled." >&2
fi

is_compiled()    { [ "${#COMPILED[@]}" -eq 0 ] || [ -n "${COMPILED[$1]:-}" ]; }
dir_compiled()   { [ "${#COMPILED[@]}" -eq 0 ] || [ -n "${COMPILED_DIR[$(dirname "$1")]:-}" ]; }

# Collect & classify files (relative paths).
CFILES=(); CFILES_NONE=(); HFILES=(); HFILES_NONE=()
while read -r f; do if is_compiled "$f"; then CFILES+=("$f"); else CFILES_NONE+=("$f"); fi; done \
  < <(for d in $DIRS; do find "code/$d" -name '*.c' 2>/dev/null; done | sort)
while read -r f; do if dir_compiled "$f"; then HFILES+=("$f"); else HFILES_NONE+=("$f"); fi; done \
  < <(for d in $DIRS; do find "code/$d" -name '*.h' 2>/dev/null; done | sort)

bs() { printf '%s' "$1" | sed 's#/#\\#g'; }

vcxproj="$NAME.vcxproj"
filters="$NAME.vcxproj.filters"
sln="$NAME.sln"

DEFS_X64='ELITEFORCE;BOTLIB;USE_RENDERER_DLOPEN;USE_OPENAL;USE_OPENAL_DLOPEN;USE_CURL;USE_CURL_DLOPEN;USE_VOIP;USE_CODEC_VORBIS;USE_CODEC_OPUS;USE_LOCAL_HEADERS;USE_INTERNAL_JPEG;PRODUCT_VERSION="1.38";ARCH_STRING="x86_64";WIN32;_WIN32;__MINGW32__;$(NMakePreprocessorDefinitions)'
DEFS_X86='ELITEFORCE;BOTLIB;USE_RENDERER_DLOPEN;USE_OPENAL;USE_OPENAL_DLOPEN;USE_CURL;USE_CURL_DLOPEN;USE_VOIP;USE_CODEC_VORBIS;USE_CODEC_OPUS;USE_LOCAL_HEADERS;USE_INTERNAL_JPEG;PRODUCT_VERSION="1.38";ARCH_STRING="x86";WIN32;_WIN32;__MINGW32__;$(NMakePreprocessorDefinitions)'
INCS='$(ProjectDir)code;$(ProjectDir)code\qcommon;$(ProjectDir)code\client;$(ProjectDir)code\server;$(ProjectDir)code\renderercommon;$(ProjectDir)code\botlib;$(ProjectDir)code\SDL2\include;$(ProjectDir)code\zlib;$(ProjectDir)code\jpeg-8c;$(ProjectDir)code\libcurl-7.35.0\include;$(NMakeIncludeSearchPath)'

BASH='C:\msys64\usr\bin\bash.exe'
FLAGS_X64='ARCH=x86_64 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres'
FLAGS_X86='ARCH=x86 BUILD_ELITEFORCE=1 BUILD_MISSIONPACK=0 BUILD_SERVER=0 BUILD_GAME_QVM=0 WINDRES=windres'

# Build a per-config NMake PropertyGroup block.
# args: condition  msystem  maketarget  cleantarget  flags  output  defs
nmake_props() {
  local cond="$1" msys="$2" target="$3" cleant="$4" flags="$5" out="$6" defs="$7"
  cat <<EOF
  <PropertyGroup Condition="$cond">
    <NMakeBuildCommandLine>set MSYSTEM=$msys
set MSYS2_PATH_TYPE=inherit
$BASH -lc "cd '$REPO_MSYS' &amp;&amp; make $target $flags -j8"</NMakeBuildCommandLine>
    <NMakeReBuildCommandLine>set MSYSTEM=$msys
set MSYS2_PATH_TYPE=inherit
$BASH -lc "cd '$REPO_MSYS' &amp;&amp; make $cleant $flags &amp;&amp; make $target $flags -j8"</NMakeReBuildCommandLine>
    <NMakeCleanCommandLine>set MSYSTEM=$msys
set MSYS2_PATH_TYPE=inherit
$BASH -lc "cd '$REPO_MSYS' &amp;&amp; make $cleant $flags"</NMakeCleanCommandLine>
    <NMakeOutput>$out</NMakeOutput>
    <NMakePreprocessorDefinitions>$defs</NMakePreprocessorDefinitions>
    <NMakeIncludeSearchPath>$INCS</NMakeIncludeSearchPath>
  </PropertyGroup>
EOF
}

############################ vcxproj ############################
{
cat <<EOF
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="15.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Debug|Win32"><Configuration>Debug</Configuration><Platform>Win32</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|Win32"><Configuration>Release</Configuration><Platform>Win32</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>$PROJ_GUID</ProjectGuid>
    <Keyword>MakeFileProj</Keyword>
    <ProjectName>$NAME</ProjectName>
  </PropertyGroup>
  <Import Project="\$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'\$(Configuration)'=='Release'" Label="Configuration">
    <ConfigurationType>Makefile</ConfigurationType><UseDebugLibraries>false</UseDebugLibraries><PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <PropertyGroup Condition="'\$(Configuration)'=='Debug'" Label="Configuration">
    <ConfigurationType>Makefile</ConfigurationType><UseDebugLibraries>true</UseDebugLibraries><PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <Import Project="\$(VCTargetsPath)\Microsoft.Cpp.props" />
EOF

nmake_props "'\$(Configuration)|\$(Platform)'=='Release|x64'"  "MINGW64" ""      "clean-release" "$FLAGS_X64" 'build\release-mingw64-x86_64\ioquake3.x86_64.exe' "$DEFS_X64"
nmake_props "'\$(Configuration)|\$(Platform)'=='Debug|x64'"    "MINGW64" "debug" "clean-debug"   "$FLAGS_X64" 'build\debug-mingw64-x86_64\ioquake3.x86_64.exe'   "$DEFS_X64"
nmake_props "'\$(Configuration)|\$(Platform)'=='Release|Win32'" "MINGW32" ""      "clean-release" "$FLAGS_X86" 'build\release-mingw32-x86\ioquake3.x86.exe'       "$DEFS_X86"
nmake_props "'\$(Configuration)|\$(Platform)'=='Debug|Win32'"   "MINGW32" "debug" "clean-debug"   "$FLAGS_X86" 'build\debug-mingw32-x86\ioquake3.x86.exe'         "$DEFS_X86"

echo '  <ItemGroup>'
for f in "${CFILES[@]}"; do echo "    <ClCompile Include=\"$(bs "$f")\" />"; done
echo '  </ItemGroup>'
echo '  <ItemGroup>'
for f in "${HFILES[@]}"; do echo "    <ClInclude Include=\"$(bs "$f")\" />"; done
echo '  </ItemGroup>'
# Browse-only items: not part of the Windows build, so kept out of IntelliSense.
echo '  <ItemGroup>'
for f in "${CFILES_NONE[@]}"; do echo "    <None Include=\"$(bs "$f")\" />"; done
for f in "${HFILES_NONE[@]}"; do echo "    <None Include=\"$(bs "$f")\" />"; done
echo '  </ItemGroup>'
cat <<EOF
  <Import Project="\$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
EOF
} > "$vcxproj"

############################ filters ############################
# Unique filter folders = directory of each file.
mapfile -t FOLDERS < <(for f in "${CFILES[@]}" "${HFILES[@]}" "${CFILES_NONE[@]}" "${HFILES_NONE[@]}"; do dirname "$f"; done | sort -u | while read -r d; do
  # emit each ancestor path too so the tree is complete
  IFS='/' read -ra parts <<<"$d"; acc=""; for p in "${parts[@]}"; do acc="${acc:+$acc/}$p"; echo "$acc"; done
done | sort -u)

guid_for() { # deterministic-ish pseudo guid from path hash
  local h; h=$(printf '%s' "$1" | cksum | cut -d' ' -f1)
  printf '{%08X-0000-0000-0000-000000000000}' "$h"
}

{
cat <<EOF
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup>
EOF
for d in "${FOLDERS[@]}"; do
  echo "    <Filter Include=\"$(bs "$d")\"><UniqueIdentifier>$(guid_for "$d")</UniqueIdentifier></Filter>"
done
echo '  </ItemGroup>'
echo '  <ItemGroup>'
for f in "${CFILES[@]}"; do echo "    <ClCompile Include=\"$(bs "$f")\"><Filter>$(bs "$(dirname "$f")")</Filter></ClCompile>"; done
echo '  </ItemGroup>'
echo '  <ItemGroup>'
for f in "${HFILES[@]}"; do echo "    <ClInclude Include=\"$(bs "$f")\"><Filter>$(bs "$(dirname "$f")")</Filter></ClInclude>"; done
echo '  </ItemGroup>'
echo '  <ItemGroup>'
for f in "${CFILES_NONE[@]}" "${HFILES_NONE[@]}"; do echo "    <None Include=\"$(bs "$f")\"><Filter>$(bs "$(dirname "$f")")</Filter></None>"; done
echo '  </ItemGroup>'
echo '</Project>'
} > "$filters"

############################ sln ############################
cat > "$sln" <<EOF
Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
VisualStudioVersion = 17.0.0.0
MinimumVisualStudioVersion = 10.0.40219.1
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "$NAME", "$NAME.vcxproj", "$PROJ_GUID"
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|x64 = Debug|x64
		Release|x64 = Release|x64
		Debug|Win32 = Debug|Win32
		Release|Win32 = Release|Win32
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		$PROJ_GUID.Debug|x64.ActiveCfg = Debug|x64
		$PROJ_GUID.Debug|x64.Build.0 = Debug|x64
		$PROJ_GUID.Release|x64.ActiveCfg = Release|x64
		$PROJ_GUID.Release|x64.Build.0 = Release|x64
		$PROJ_GUID.Debug|Win32.ActiveCfg = Debug|Win32
		$PROJ_GUID.Debug|Win32.Build.0 = Debug|Win32
		$PROJ_GUID.Release|Win32.ActiveCfg = Release|Win32
		$PROJ_GUID.Release|Win32.Build.0 = Release|Win32
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
	GlobalSection(ExtensibilityGlobals) = postSolution
		SolutionGuid = $SLN_GUID
	EndGlobalSection
EndGlobal
EOF

echo "Generated: $vcxproj ($(wc -l < "$vcxproj") lines), $filters, $sln"
echo "Compiled (IntelliSense): ${#CFILES[@]} .c, ${#HFILES[@]} .h"
echo "Browse-only (<None>):    ${#CFILES_NONE[@]} .c, ${#HFILES_NONE[@]} .h"
