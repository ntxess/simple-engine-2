# Audio tests (docs/milestone-specs.md M8 "Audio"; test names start with
# "audio."). se_audio and the runtime's AudioBridge, mixed in miniaudio's
# no-device mode or on its null backend, so nothing is ever heard: label
# unit, everywhere, including the WSL ASan and TSan builds. The one test
# that opens the real playback device does so at volume 0 (label desktop).
#
# The AudioBridge lives in se_runtime, which defines the real WakeMain(),
# so this binary does not link se_test_support (it has the null one).

se_add_module(se_audio_tests KIND EXECUTABLE
  SOURCES
    audio_test.cc
    audio_desktop_test.cc
    sound_assets_test.cc
    ../support/no_dialogs_listener.cc
    ../support/tracy_inert_listener.cc
  PRIVATE_DEPS
    se_audio
    se_core
    se_runtime
    se_sim
    Catch2::Catch2WithMain
  TEST)
target_include_directories(se_audio_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
# The integration's lane tests write their WAV files here.
set(_se_audio_out "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${_se_audio_out}")
target_compile_definitions(se_audio_tests PRIVATE
  SE_TEST_OUT_DIR="${_se_audio_out}")

catch_discover_tests(se_audio_tests
  TEST_SPEC "~[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "audio."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS unit
    ENVIRONMENT SE_NO_MESSAGE_BOX=1)
catch_discover_tests(se_audio_tests
  TEST_SPEC "[desktop]"
  DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "audio."
  ADD_TAGS_AS_LABELS
  PROPERTIES
    LABELS desktop
    ENVIRONMENT SE_NO_MESSAGE_BOX=1
    TIMEOUT 60)
