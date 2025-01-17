#include <atomic>

#include "source/common/common/assert.h"

#include "test/common/http/common.h"
#include "test/common/mocks/common/mocks.h"

#include "absl/synchronization/notification.h"
#include "gtest/gtest.h"
#include "library/cc/engine_builder.h"
#include "library/common/api/external.h"
#include "library/common/bridge/utility.h"
#include "library/common/data/utility.h"
#include "library/common/http/header_utility.h"
#include "library/common/internal_engine.h"

namespace Envoy {

using testing::_;
using testing::HasSubstr;
using testing::Return;
using testing::ReturnRef;

// This config is the minimal envoy mobile config that allows for running the engine.
const std::string MINIMAL_TEST_CONFIG = R"(
listener_manager:
    name: envoy.listener_manager_impl.api
    typed_config:
      "@type": type.googleapis.com/envoy.config.listener.v3.ApiListenerManager
static_resources:
  listeners:
  - name: base_api_listener
    address:
      socket_address: { protocol: TCP, address: 0.0.0.0, port_value: 10000 }
    api_listener:
      api_listener:
        "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.EnvoyMobileHttpConnectionManager
        config:
          stat_prefix: hcm
          route_config:
            name: api_router
            virtual_hosts:
            - name: api
              include_attempt_count_in_response: true
              domains: ["*"]
              routes:
              - match: { prefix: "/" }
                route:
                  cluster_header: x-envoy-mobile-cluster
                  retry_policy:
                    retry_back_off: { base_interval: 0.25s, max_interval: 60s }
          http_filters:
          - name: envoy.router
            typed_config:
              "@type": type.googleapis.com/envoy.extensions.filters.http.router.v3.Router
layered_runtime:
  layers:
  - name: static_layer_0
    static_layer:
      overload: { global_downstream_max_connections: 50000 }
)";

const std::string BUFFERED_TEST_CONFIG = R"(
listener_manager:
    name: envoy.listener_manager_impl.api
    typed_config:
      "@type": type.googleapis.com/envoy.config.listener.v3.ApiListenerManager
static_resources:
  listeners:
  - name: base_api_listener
    address:
      socket_address: { protocol: TCP, address: 0.0.0.0, port_value: 10000 }
    api_listener:
       api_listener:
        "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.EnvoyMobileHttpConnectionManager
        config:
          stat_prefix: hcm
          route_config:
            name: api_router
            virtual_hosts:
            - name: api
              include_attempt_count_in_response: true
              domains: ["*"]
              routes:
              - match: { prefix: "/" }
                direct_response: { status: 200 }
          http_filters:
          - name: buffer
            typed_config:
              "@type": type.googleapis.com/envoy.extensions.filters.http.buffer.v3.Buffer
              max_request_bytes: 65000
          - name: envoy.router
            typed_config:
              "@type": type.googleapis.com/envoy.extensions.filters.http.router.v3.Router
layered_runtime:
  layers:
  - name: static_layer_0
    static_layer:
      overload: { global_downstream_max_connections: 50000 }
)";

const std::string LEVEL_DEBUG = "debug";

struct EngineTestContext {
  absl::Notification on_engine_running;
  absl::Notification on_exit;
  absl::Notification on_log;
  absl::Notification on_logger_release;
  absl::Notification on_event;
};

// RAII wrapper for the engine, ensuring that we properly shut down the engine. If the engine
// thread is not torn down, we end up with TSAN failures during shutdown due to a data race
// between the main thread and the engine thread both writing to the
// Envoy::Logger::current_log_context global.
struct TestEngine {
  std::unique_ptr<InternalEngine> engine_;
  envoy_engine_t handle() const { return reinterpret_cast<envoy_engine_t>(engine_.get()); }
  TestEngine(envoy_engine_callbacks callbacks, const std::string& level) {
    engine_.reset(new Envoy::InternalEngine(callbacks, {}, {}));
    Platform::EngineBuilder builder;
    auto bootstrap = builder.generateBootstrap();
    std::string yaml = Envoy::MessageUtil::getYamlStringFromMessage(*bootstrap);
    engine_->run(yaml, level);
  }

  envoy_status_t terminate() const { return engine_->terminate(); }
  [[nodiscard]] bool isTerminated() const { return engine_->isTerminated(); }

  ~TestEngine() {
    if (!engine_->isTerminated()) {
      engine_->terminate();
    }
  }
};

// Transform C map to C++ map.
[[maybe_unused]] static inline std::map<std::string, std::string> toMap(envoy_map map) {
  std::map<std::string, std::string> new_map;
  for (envoy_map_size_t i = 0; i < map.length; i++) {
    envoy_map_entry header = map.entries[i];
    const auto key = Data::Utility::copyToString(header.key);
    const auto value = Data::Utility::copyToString(header.value);
    new_map.emplace(key, value);
  }

  release_envoy_map(map);
  return new_map;
}

// Based on Http::Utility::toRequestHeaders() but only used for these tests.
Http::ResponseHeaderMapPtr toResponseHeaders(envoy_headers headers) {
  Http::ResponseHeaderMapPtr transformed_headers = Http::ResponseHeaderMapImpl::create();
  for (envoy_map_size_t i = 0; i < headers.length; i++) {
    transformed_headers->addCopy(
        Http::LowerCaseString(Data::Utility::copyToString(headers.entries[i].key)),
        Data::Utility::copyToString(headers.entries[i].value));
  }
  // The C envoy_headers struct can be released now because the headers have been copied.
  release_envoy_headers(headers);
  return transformed_headers;
}

class InternalEngineTest : public testing::Test {
public:
  void SetUp() override {
    helper_handle_ = test::SystemHelperPeer::replaceSystemHelper();
    EXPECT_CALL(helper_handle_->mock_helper(), isCleartextPermitted(_))
        .WillRepeatedly(Return(true));
  }

  std::unique_ptr<TestEngine> engine_;

private:
  std::unique_ptr<test::SystemHelperPeer::Handle> helper_handle_;
};

TEST_F(InternalEngineTest, EarlyExit) {
  const std::string level = "debug";

  EngineTestContext test_context{};
  envoy_engine_callbacks callbacks{[](void* context) -> void {
                                     auto* engine_running =
                                         static_cast<EngineTestContext*>(context);
                                     engine_running->on_engine_running.Notify();
                                   } /*on_engine_running*/,
                                   [](void* context) -> void {
                                     auto* exit = static_cast<EngineTestContext*>(context);
                                     exit->on_exit.Notify();
                                   } /*on_exit*/,
                                   &test_context /*context*/};

  engine_ = std::make_unique<TestEngine>(callbacks, level);
  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(10)));

  ASSERT_EQ(engine_->terminate(), ENVOY_SUCCESS);
  ASSERT_TRUE(engine_->isTerminated());
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(10)));

  engine_->engine_->startStream(0, {}, false);

  engine_.reset();
}

TEST_F(InternalEngineTest, AccessEngineAfterInitialization) {
  const std::string level = "debug";

  EngineTestContext test_context{};
  envoy_engine_callbacks callbacks{[](void* context) -> void {
                                     auto* engine_running =
                                         static_cast<EngineTestContext*>(context);
                                     engine_running->on_engine_running.Notify();
                                   } /*on_engine_running*/,
                                   [](void*) -> void {} /*on_exit*/, &test_context /*context*/};

  engine_ = std::make_unique<TestEngine>(callbacks, level);
  engine_->handle();
  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(10)));

  absl::Notification getClusterManagerInvoked;
  // Running engine functions should work because the engine is running
  EXPECT_EQ("runtime.load_success: 1\n", engine_->engine_->dumpStats());

  engine_->terminate();
  ASSERT_TRUE(engine_->isTerminated());

  // Now that the engine has been shut down, we no longer expect scheduling to work.
  EXPECT_EQ("", engine_->engine_->dumpStats());

  engine_.reset();
}

TEST_F(InternalEngineTest, RecordCounter) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* engine_running =
                                          static_cast<EngineTestContext*>(context);
                                      engine_running->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* exit = static_cast<EngineTestContext*>(context);
                                      exit->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};
  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, {}, {}));

  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);
  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));
  EXPECT_EQ(ENVOY_SUCCESS, engine->recordCounterInc("counter", envoy_stats_notags, 1));

  engine->terminate();
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, Logger) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};

  envoy_logger logger{[](envoy_log_level, envoy_data data, const void* context) -> void {
                        auto* test_context =
                            static_cast<EngineTestContext*>(const_cast<void*>(context));
                        release_envoy_data(data);
                        if (!test_context->on_log.HasBeenNotified()) {
                          test_context->on_log.Notify();
                        }
                      } /* log */,
                      [](const void* context) -> void {
                        auto* test_context =
                            static_cast<EngineTestContext*>(const_cast<void*>(context));
                        test_context->on_logger_release.Notify();
                      } /* release */,
                      &test_context};
  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, logger, {}));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);
  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));

  ASSERT_TRUE(test_context.on_log.WaitForNotificationWithTimeout(absl::Seconds(3)));

  engine->terminate();
  engine.reset();
  ASSERT_TRUE(test_context.on_logger_release.WaitForNotificationWithTimeout(absl::Seconds(3)));
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, EventTrackerRegistersDefaultAPI) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};

  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, {}, {}));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);

  // A default event tracker is registered in external API registry.
  const auto registered_event_tracker =
      static_cast<envoy_event_tracker*>(Api::External::retrieveApi(envoy_event_tracker_api_name));
  EXPECT_TRUE(registered_event_tracker != nullptr);
  EXPECT_TRUE(registered_event_tracker->track == nullptr);
  EXPECT_TRUE(registered_event_tracker->context == nullptr);

  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));
  // Simulate a failed assertion by invoking a debug assertion failure
  // record action.
  // Verify that no crash if the assertion fails when no real event
  // tracker is passed at engine's initialization time.
  Assert::invokeDebugAssertionFailureRecordActionForAssertMacroUseOnly("foo_location");

  engine->terminate();
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, EventTrackerRegistersAPI) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};
  envoy_event_tracker event_tracker{[](envoy_map map, const void* context) -> void {
                                      const auto new_map = toMap(map);
                                      if (new_map.count("foo") && new_map.at("foo") == "bar") {
                                        auto* test_context = static_cast<EngineTestContext*>(
                                            const_cast<void*>(context));
                                        test_context->on_event.Notify();
                                      }
                                    } /*track*/,
                                    &test_context /*context*/};

  std::unique_ptr<Envoy::InternalEngine> engine(
      new Envoy::InternalEngine(engine_cbs, {}, event_tracker));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);

  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));
  const auto registered_event_tracker =
      static_cast<envoy_event_tracker*>(Api::External::retrieveApi(envoy_event_tracker_api_name));
  EXPECT_TRUE(registered_event_tracker != nullptr);
  EXPECT_EQ(event_tracker.track, registered_event_tracker->track);
  EXPECT_EQ(event_tracker.context, registered_event_tracker->context);

  event_tracker.track(Bridge::Utility::makeEnvoyMap({{"foo", "bar"}}),
                      registered_event_tracker->context);

  ASSERT_TRUE(test_context.on_event.WaitForNotificationWithTimeout(absl::Seconds(3)));
  engine->terminate();
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, EventTrackerRegistersAssertionFailureRecordAction) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};

  envoy_event_tracker event_tracker{
      [](envoy_map map, const void* context) -> void {
        const auto new_map = toMap(map);
        if (new_map.count("name") && new_map.at("name") == "assertion") {
          EXPECT_EQ(new_map.at("location"), "foo_location");
          auto* test_context = static_cast<EngineTestContext*>(const_cast<void*>(context));
          test_context->on_event.Notify();
        }
      } /*track*/,
      &test_context /*context*/};

  std::unique_ptr<Envoy::InternalEngine> engine(
      new Envoy::InternalEngine(engine_cbs, {}, event_tracker));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);

  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));
  // Simulate a failed assertion by invoking a debug assertion failure
  // record action.
  // Verify that an envoy event is emitted when an event tracker is passed
  // at engine's initialization time.
  Assert::invokeDebugAssertionFailureRecordActionForAssertMacroUseOnly("foo_location");

  ASSERT_TRUE(test_context.on_event.WaitForNotificationWithTimeout(absl::Seconds(3)));
  engine->terminate();
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, EventTrackerRegistersEnvoyBugRecordAction) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* test_context = static_cast<EngineTestContext*>(context);
                                      test_context->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};

  envoy_event_tracker event_tracker{[](envoy_map map, const void* context) -> void {
                                      const auto new_map = toMap(map);
                                      if (new_map.count("name") && new_map.at("name") == "bug") {
                                        EXPECT_EQ(new_map.at("location"), "foo_location");
                                        auto* test_context = static_cast<EngineTestContext*>(
                                            const_cast<void*>(context));
                                        test_context->on_event.Notify();
                                      }
                                    } /*track*/,
                                    &test_context /*context*/};

  std::unique_ptr<Envoy::InternalEngine> engine(
      new Envoy::InternalEngine(engine_cbs, {}, event_tracker));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);

  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));
  // Simulate an envoy bug by invoking an Envoy bug failure
  // record action.
  // Verify that an envoy event is emitted when an event tracker is passed
  // at engine's initialization time.
  Assert::invokeEnvoyBugFailureRecordActionForEnvoyBugMacroUseOnly("foo_location");

  ASSERT_TRUE(test_context.on_event.WaitForNotificationWithTimeout(absl::Seconds(3)));
  engine->terminate();
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, BasicStream) {
  const std::string level = "debug";
  EngineTestContext engine_cbs_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* engine_running =
                                          static_cast<EngineTestContext*>(context);
                                      engine_running->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* exit = static_cast<EngineTestContext*>(context);
                                      exit->on_exit.Notify();
                                    } /*on_exit*/,
                                    &engine_cbs_context /*context*/};
  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, {}, {}));
  engine->run(BUFFERED_TEST_CONFIG, level);

  ASSERT_TRUE(
      engine_cbs_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(10)));

  absl::Notification on_complete_notification;
  envoy_http_callbacks stream_cbs{
      [](envoy_headers c_headers, bool end_stream, envoy_stream_intel, void*) -> void {
        auto response_headers = toResponseHeaders(c_headers);
        EXPECT_EQ(response_headers->Status()->value().getStringView(), "200");
        EXPECT_TRUE(end_stream);
      } /* on_headers */,
      nullptr /* on_data */,
      nullptr /* on_metadata */,
      nullptr /* on_trailers */,
      nullptr /* on_error */,
      [](envoy_stream_intel, envoy_final_stream_intel, void* context) -> void {
        auto* on_complete_notification = static_cast<absl::Notification*>(context);
        on_complete_notification->Notify();
      } /* on_complete */,
      nullptr /* on_cancel */,
      nullptr /* on_send_window_available*/,
      &on_complete_notification /* context */};
  Http::TestRequestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  envoy_headers c_headers = Http::Utility::toBridgeHeaders(headers);

  Buffer::OwnedImpl request_data = Buffer::OwnedImpl("request body");
  envoy_data c_data = Data::Utility::toBridgeData(request_data);

  Http::TestRequestTrailerMapImpl trailers;
  envoy_headers c_trailers = Http::Utility::toBridgeHeaders(trailers);

  envoy_stream_t stream = engine->initStream();

  engine->startStream(stream, stream_cbs, false);

  engine->sendHeaders(stream, c_headers, false);
  engine->sendData(stream, c_data, false);
  engine->sendTrailers(stream, c_trailers);

  ASSERT_TRUE(on_complete_notification.WaitForNotificationWithTimeout(absl::Seconds(10)));

  engine->terminate();

  ASSERT_TRUE(engine_cbs_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(10)));
}

TEST_F(InternalEngineTest, ResetStream) {
  EngineTestContext engine_cbs_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* engine_running =
                                          static_cast<EngineTestContext*>(context);
                                      engine_running->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* exit = static_cast<EngineTestContext*>(context);
                                      exit->on_exit.Notify();
                                    } /*on_exit*/,
                                    &engine_cbs_context /*context*/};

  // There is nothing functional about the config used to run the engine, as the created stream is
  // immediately reset.
  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, {}, {}));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);

  ASSERT_TRUE(
      engine_cbs_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(10)));

  absl::Notification on_cancel_notification;
  envoy_http_callbacks stream_cbs{
      nullptr /* on_headers */,
      nullptr /* on_data */,
      nullptr /* on_metadata */,
      nullptr /* on_trailers */,
      nullptr /* on_error */,
      nullptr /* on_complete */,
      [](envoy_stream_intel, envoy_final_stream_intel, void* context) -> void {
        auto* on_cancel_notification = static_cast<absl::Notification*>(context);
        on_cancel_notification->Notify();
      } /* on_cancel */,
      nullptr /* on_send_window_available */,
      &on_cancel_notification /* context */};

  envoy_stream_t stream = engine->initStream();

  engine->startStream(stream, stream_cbs, false);

  engine->cancelStream(stream);

  ASSERT_TRUE(on_cancel_notification.WaitForNotificationWithTimeout(absl::Seconds(10)));

  engine->terminate();

  ASSERT_TRUE(engine_cbs_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(10)));
}

TEST_F(InternalEngineTest, RegisterPlatformApi) {
  EngineTestContext engine_cbs_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* engine_running =
                                          static_cast<EngineTestContext*>(context);
                                      engine_running->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* exit = static_cast<EngineTestContext*>(context);
                                      exit->on_exit.Notify();
                                    } /*on_exit*/,
                                    &engine_cbs_context /*context*/};

  // Using the minimal envoy mobile config that allows for running the engine.
  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, {}, {}));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);

  ASSERT_TRUE(
      engine_cbs_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(10)));

  uint64_t fake_api;
  Envoy::Api::External::registerApi("api", &fake_api);

  engine->terminate();

  ASSERT_TRUE(engine_cbs_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(10)));
}

TEST_F(InternalEngineTest, ResetConnectivityState) {
  EngineTestContext test_context{};
  envoy_engine_callbacks engine_cbs{[](void* context) -> void {
                                      auto* engine_running =
                                          static_cast<EngineTestContext*>(context);
                                      engine_running->on_engine_running.Notify();
                                    } /*on_engine_running*/,
                                    [](void* context) -> void {
                                      auto* exit = static_cast<EngineTestContext*>(context);
                                      exit->on_exit.Notify();
                                    } /*on_exit*/,
                                    &test_context /*context*/};
  std::unique_ptr<Envoy::InternalEngine> engine(new Envoy::InternalEngine(engine_cbs, {}, {}));
  engine->run(MINIMAL_TEST_CONFIG, LEVEL_DEBUG);
  ASSERT_TRUE(test_context.on_engine_running.WaitForNotificationWithTimeout(absl::Seconds(3)));

  ASSERT_EQ(ENVOY_SUCCESS, engine->resetConnectivityState());

  engine->terminate();
  ASSERT_TRUE(test_context.on_exit.WaitForNotificationWithTimeout(absl::Seconds(3)));
}

TEST_F(InternalEngineTest, SetLogger) {
  std::atomic<bool> logging_was_called{false};
  envoy_logger logger;
  logger.log = [](envoy_log_level, envoy_data data, const void* context) {
    std::atomic<bool>* logging_was_called =
        const_cast<std::atomic<bool>*>(static_cast<const std::atomic<bool>*>(context));
    *logging_was_called = true;
    release_envoy_data(data);
  };
  logger.release = envoy_noop_const_release;
  logger.context = &logging_was_called;

  absl::Notification engine_running;
  Platform::EngineBuilder engine_builder;
  Platform::EngineSharedPtr engine =
      engine_builder.addLogLevel(Platform::LogLevel::debug)
          .setLogger(logger)
          .setOnEngineRunning([&] { engine_running.Notify(); })
          .addNativeFilter(
              "test_remote_response",
              "{'@type': "
              "type.googleapis.com/"
              "envoymobile.extensions.filters.http.test_remote_response.TestRemoteResponse}")
          .build();
  engine_running.WaitForNotification();

  int actual_status_code = 0;
  bool actual_end_stream = false;
  absl::Notification stream_complete;
  auto stream_prototype = engine->streamClient()->newStreamPrototype();
  auto stream = (*stream_prototype)
                    .setOnHeaders([&](Platform::ResponseHeadersSharedPtr headers, bool end_stream,
                                      envoy_stream_intel) {
                      actual_status_code = headers->httpStatus();
                      actual_end_stream = end_stream;
                    })
                    .setOnData([&](envoy_data data, bool end_stream) {
                      actual_end_stream = end_stream;
                      release_envoy_data(data);
                    })
                    .setOnComplete([&](envoy_stream_intel, envoy_final_stream_intel) {
                      stream_complete.Notify();
                    })
                    .setOnError([&](Platform::EnvoyErrorSharedPtr, envoy_stream_intel,
                                    envoy_final_stream_intel) { stream_complete.Notify(); })
                    .setOnCancel([&](envoy_stream_intel, envoy_final_stream_intel) {
                      stream_complete.Notify();
                    })
                    .start();

  auto request_headers =
      Platform::RequestHeadersBuilder(Platform::RequestMethod::GET, "https", "example.com", "/")
          .build();
  stream->sendHeaders(std::make_shared<Platform::RequestHeaders>(request_headers), true);
  stream_complete.WaitForNotification();

  EXPECT_EQ(actual_status_code, 200);
  EXPECT_EQ(actual_end_stream, true);
  EXPECT_TRUE(logging_was_called.load());
  EXPECT_EQ(engine->terminate(), ENVOY_SUCCESS);
}

} // namespace Envoy
