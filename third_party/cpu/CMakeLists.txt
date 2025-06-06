# Find OneDNN ukernel library
find_package(dnnl CONFIG)
if (dnnl_FOUND)
  message(STATUS "Found OneDNN/DNNL")
  add_compile_definitions(ONEDNN_AVAILABLE)
  get_target_property(dnnl_include DNNL::dnnl INTERFACE_INCLUDE_DIRECTORIES)
  # currently used only in triton_cpu.cc and in ConvertDotToOneDNN
  include_directories(${dnnl_include})
else ()
  message(STATUS "Could NOT find OneDNN/DNNL")
endif()

# Find XSMM ukernel library
find_library(LIBXSMM xsmm
  PATHS ENV XSMM_LIBRARY_DIRS
)
find_path(LIBXSMM_INCLUDE_DIR
  NAMES libxsmm.h
  PATHS ENV XSMM_INCLUDE_DIRS
)
if (LIBXSMM AND LIBXSMM_INCLUDE_DIR)
  set(xsmm_FOUND True)
  message(STATUS "Found XSMM: " ${LIBXSMM})
  add_compile_definitions(XSMM_AVAILABLE)
  include_directories(${LIBXSMM_INCLUDE_DIR})
else ()
  message(STATUS "Could NOT find XSMM")
endif()

# Build Triton-CPU plugin
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)
add_subdirectory(include)
add_subdirectory(lib)
if(TRITON_BUILD_PYTHON_MODULE)
  add_triton_plugin(TritonCPU ${CMAKE_CURRENT_SOURCE_DIR}/triton_cpu.cc LINK_LIBS TritonCPUToLLVM TritonCPUTransforms)
  target_link_libraries(TritonCPU PUBLIC MLIRVectorToSCF MLIRAffineToStandard MLIRMathToLibm MLIRAMXToLLVMIRTranslation MLIRMemRefTransforms MLIRReconcileUnrealizedCasts PRIVATE Python3::Module pybind11::headers)
endif()

# Configure and build Triton-CPU runtime
set(TRITON_CPU_RUNTIME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/runtime/cpu_runtime.cpp)
set(TRITON_CPU_RUNTIME_LIBS LLVMSupport)
if (dnnl_FOUND)
  set(TRITON_CPU_RUNTIME_SOURCES ${TRITON_CPU_RUNTIME_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/runtime/runtime_onednn.cpp)
  set(TRITON_CPU_RUNTIME_LIBS ${TRITON_CPU_RUNTIME_LIBS} DNNL::dnnl)
endif()
if(xsmm_FOUND)
  set(TRITON_CPU_RUNTIME_SOURCES ${TRITON_CPU_RUNTIME_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/runtime/runtime_xsmm.cpp)
  set(TRITON_CPU_RUNTIME_LIBS ${TRITON_CPU_RUNTIME_LIBS} ${LIBXSMM})
endif()

add_library(TritonCPURuntime SHARED ${TRITON_CPU_RUNTIME_SOURCES})
target_link_libraries(TritonCPURuntime PRIVATE ${TRITON_CPU_RUNTIME_LIBS})

# Build and link sleef
set(SLEEF_BUILD_SHARED_LIBS ON CACHE BOOL "Build sleef shared lib" FORCE)
set(SLEEF_BUILD_DFT OFF CACHE BOOL "Don't build sleef DFT lib" FORCE)
set(SLEEF_BUILD_GNUABI_LIBS OFF CACHE BOOL "Don't build sleef gnuabi libs" FORCE)
set(SLEEF_BUILD_TESTS OFF CACHE BOOL "Don't build sleef tests" FORCE)
set(SLEEF_BUILD_SCALAR_LIB OFF CACHE BOOL "libsleefscalar will not be built." FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/sleef" sleef)
# Override sleef's output directory with our own
set_target_properties(sleef PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
