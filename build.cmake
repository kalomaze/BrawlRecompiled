cmake_minimum_required(VERSION 3.25)
# Run from any directory, after extract.cmake: cmake -P /path/to/build.cmake
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(args --cmake "${CMAKE_COMMAND}")
foreach(var BUILD_DIR GAME EXTRACT JOBS)  # build.py's own options: -DJOBS=2 -> --jobs=2
    if(DEFINED ${var})
        string(TOLOWER "${var}" option)
        string(REPLACE "_" "-" option "${option}")
        list(APPEND args "--${option}=${${var}}")
    endif()
endforeach()
foreach(var CMAKE_BUILD_TYPE MKW_HOST_ISA MKW_FLAT_MEMORY)  # the native configure's, like -DNATIVE_ARGS="-DA=1;-DB=2"
    if(DEFINED ${var})
        list(APPEND NATIVE_ARGS "-D${var}=${${var}}")
    endif()
endforeach()
foreach(arg IN LISTS NATIVE_ARGS)
    list(APPEND args "--native-arg=${arg}")
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/build.py" ${args}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Build failed (${result}); see the step output above.")
endif()
