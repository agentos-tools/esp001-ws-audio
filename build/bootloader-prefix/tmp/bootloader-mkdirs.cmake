# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/mx/esp/esp-idf/components/bootloader/subproject"
  "/home/mx/esp/esp001-audio/build/bootloader"
  "/home/mx/esp/esp001-audio/build/bootloader-prefix"
  "/home/mx/esp/esp001-audio/build/bootloader-prefix/tmp"
  "/home/mx/esp/esp001-audio/build/bootloader-prefix/src/bootloader-stamp"
  "/home/mx/esp/esp001-audio/build/bootloader-prefix/src"
  "/home/mx/esp/esp001-audio/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/mx/esp/esp001-audio/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/mx/esp/esp001-audio/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
