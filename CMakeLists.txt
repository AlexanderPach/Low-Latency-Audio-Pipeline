# CMakeList.txt : CMake project for LLAP, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.12)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("LLAP")

# Add source to this project's executable.
add_executable (LLAP "LLAP.cpp" "LLAP.h" "disruptor.h" "SPSCDisruptor.cpp" "audio_engine.h")

target_compile_features(LLAP PRIVATE cxx_std_20)

include(CTest)
if (BUILD_TESTING)
  find_package(Threads REQUIRED)

  add_executable(spsc_disruptor_tests tests/spsc_disruptor_tests.cpp)
  add_executable(audio_engine_tests tests/audio_engine_tests.cpp)

  foreach(test_target IN ITEMS spsc_disruptor_tests audio_engine_tests)
    target_compile_features(${test_target} PRIVATE cxx_std_20)
    target_include_directories(${test_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${test_target} PRIVATE Threads::Threads)
  endforeach()

  # Exercise the real callback thread without accessing hardware or playing sound.
  target_compile_definitions(audio_engine_tests PRIVATE
    MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_NULL)
  target_link_libraries(audio_engine_tests PRIVATE ${CMAKE_DL_LIBS})

  foreach(test_case IN ITEMS empty fifo capacity wraparound move_only lifetime
                             concurrent_small concurrent_audio)
    add_test(NAME spsc.${test_case} COMMAND spsc_disruptor_tests ${test_case})
    set_tests_properties(spsc.${test_case} PROPERTIES TIMEOUT 30)
  endforeach()

  foreach(test_case IN ITEMS consumes_samples restart destruction_running)
    add_test(NAME audio.${test_case} COMMAND audio_engine_tests ${test_case})
    set_tests_properties(audio.${test_case} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

option(LLAP_BUILD_BENCHMARKS "Build queue comparison and paced audio simulation" OFF)
if (LLAP_BUILD_BENCHMARKS)
  find_package(Threads REQUIRED)
  foreach(benchmark_target IN ITEMS queue_benchmark audio_queue_simulation)
    add_executable(${benchmark_target} benchmarks/${benchmark_target}.cpp)
    target_compile_features(${benchmark_target} PRIVATE cxx_std_20)
    target_include_directories(${benchmark_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${benchmark_target} PRIVATE Threads::Threads)
  endforeach()

  if (BUILD_TESTING)
    add_test(NAME benchmark.queue_correctness COMMAND queue_benchmark --self-test)
    add_test(NAME benchmark.simulation_correctness COMMAND audio_queue_simulation --quick)
    set_tests_properties(benchmark.queue_correctness benchmark.simulation_correctness
      PROPERTIES TIMEOUT 30)
  endif()
endif()
