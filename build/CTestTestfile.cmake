# CMake generated Testfile for 
# Source directory: C:/Users/Tomasz/CLionProjects/obs_multistream_plugin
# Build directory: C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[destination-rules-tests]=] "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/build/Debug/destination-rules-tests.exe")
  set_tests_properties([=[destination-rules-tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;60;add_test;C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[destination-rules-tests]=] "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/build/Release/destination-rules-tests.exe")
  set_tests_properties([=[destination-rules-tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;60;add_test;C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[destination-rules-tests]=] "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/build/MinSizeRel/destination-rules-tests.exe")
  set_tests_properties([=[destination-rules-tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;60;add_test;C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[destination-rules-tests]=] "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/build/RelWithDebInfo/destination-rules-tests.exe")
  set_tests_properties([=[destination-rules-tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;60;add_test;C:/Users/Tomasz/CLionProjects/obs_multistream_plugin/CMakeLists.txt;0;")
else()
  add_test([=[destination-rules-tests]=] NOT_AVAILABLE)
endif()
