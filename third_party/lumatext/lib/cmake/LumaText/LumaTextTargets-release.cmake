#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "LumaText::Shared" for configuration "Release"
set_property(TARGET LumaText::Shared APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(LumaText::Shared PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/lumatext.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/lumatext.dll"
  )

list(APPEND _cmake_import_check_targets LumaText::Shared )
list(APPEND _cmake_import_check_files_for_LumaText::Shared "${_IMPORT_PREFIX}/lib/lumatext.lib" "${_IMPORT_PREFIX}/bin/lumatext.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
