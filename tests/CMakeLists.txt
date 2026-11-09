include(FetchContent)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ceio_tests
  sim/test_time.cpp
  sim/test_simulator.cpp
  sim/test_rng.cpp
  sim/test_stats.cpp
  hw/test_cache.cpp
  hw/test_dram.cpp
  hw/test_memory_system.cpp
  hw/test_pcie.cpp
  net/test_net.cpp
  host/test_host.cpp
  nic/test_nic.cpp
  policy/test_credit_manager.cpp
  policy/test_ceio.cpp
  policy/test_baselines.cpp
  exp/test_experiment.cpp)
target_link_libraries(ceio_tests PRIVATE ceio::core ceio_options GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(ceio_tests DISCOVERY_TIMEOUT 60)
